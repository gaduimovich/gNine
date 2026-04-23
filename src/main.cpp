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
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <stdexcept>
#include <memory>
#include <limits>
#include <utility>

#include "Image.h"
#include "Parser.h"
#include "ImageArray.hpp"
#include "VectorProgram.hpp"
#include "JitBuilder.hpp"
#include "Runtime.hpp"
#include "RuntimeInputState.hpp"
#include "PreviewPlayback.hpp"

#ifdef GNINE_HAVE_SDL2
#include <SDL.h>
#endif

using namespace gnine;

namespace
{
   runtime::Value makeDynamicRuntimeNumber(const std::string &name, double value)
   {
      return runtime::Value::numberValue(value, Cell(Cell::Symbol, name));
   }

   void addRuntimeInputBindings(std::map<std::string, runtime::Value> *bindings,
                                const RuntimeInputState &input,
                                double previewTimeMs,
                                double previewDeltaMs)
   {
      (*bindings)["key-up"] = makeDynamicRuntimeNumber("key-up", input.keyUp);
      (*bindings)["key-down"] = makeDynamicRuntimeNumber("key-down", input.keyDown);
      (*bindings)["key-left"] = makeDynamicRuntimeNumber("key-left", input.keyLeft);
      (*bindings)["key-right"] = makeDynamicRuntimeNumber("key-right", input.keyRight);
      (*bindings)["key-w"] = makeDynamicRuntimeNumber("key-w", input.keyW);
      (*bindings)["key-a"] = makeDynamicRuntimeNumber("key-a", input.keyA);
      (*bindings)["key-s"] = makeDynamicRuntimeNumber("key-s", input.keyS);
      (*bindings)["key-d"] = makeDynamicRuntimeNumber("key-d", input.keyD);
      (*bindings)["key-space"] = makeDynamicRuntimeNumber("key-space", input.keySpace);
      (*bindings)["key-return"] = makeDynamicRuntimeNumber("key-return", input.keyReturn);
      (*bindings)["key-escape"] = makeDynamicRuntimeNumber("key-escape", input.keyEscape);
      (*bindings)["mouse-x"] = makeDynamicRuntimeNumber("mouse-x", input.mouseX);
      (*bindings)["mouse-y"] = makeDynamicRuntimeNumber("mouse-y", input.mouseY);
      (*bindings)["mouse-left"] = makeDynamicRuntimeNumber("mouse-left", input.mouseLeft);
      (*bindings)["mouse-right"] = makeDynamicRuntimeNumber("mouse-right", input.mouseRight);
      (*bindings)["mouse-wheel-y"] = makeDynamicRuntimeNumber("mouse-wheel-y", input.mouseWheelY);
      (*bindings)["key-shift"] = makeDynamicRuntimeNumber("key-shift", input.keyShift);
      (*bindings)["key-ctrl"] = makeDynamicRuntimeNumber("key-ctrl", input.keyCtrl);
      (*bindings)["key-tab"] = makeDynamicRuntimeNumber("key-tab", input.keyTab);
      (*bindings)["key-0"] = makeDynamicRuntimeNumber("key-0", input.key0);
      (*bindings)["key-1"] = makeDynamicRuntimeNumber("key-1", input.key1);
      (*bindings)["key-2"] = makeDynamicRuntimeNumber("key-2", input.key2);
      (*bindings)["key-3"] = makeDynamicRuntimeNumber("key-3", input.key3);
      (*bindings)["key-4"] = makeDynamicRuntimeNumber("key-4", input.key4);
      (*bindings)["key-5"] = makeDynamicRuntimeNumber("key-5", input.key5);
      (*bindings)["key-6"] = makeDynamicRuntimeNumber("key-6", input.key6);
      (*bindings)["key-7"] = makeDynamicRuntimeNumber("key-7", input.key7);
      (*bindings)["key-8"] = makeDynamicRuntimeNumber("key-8", input.key8);
      (*bindings)["key-9"] = makeDynamicRuntimeNumber("key-9", input.key9);
      (*bindings)["preview-time-ms"] = makeDynamicRuntimeNumber("preview-time-ms", previewTimeMs);
      (*bindings)["preview-delta-ms"] = makeDynamicRuntimeNumber("preview-delta-ms", previewDeltaMs);
   }

   int autoDisplayScaleForImage(int imageWidth, int imageHeight)
   {
      if (imageWidth <= 0 || imageHeight <= 0)
         return 1;

#ifdef GNINE_HAVE_SDL2
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
         return 1;

      int scale = 1;
      SDL_Rect bounds;
      if (SDL_GetDisplayUsableBounds(0, &bounds) != 0)
      {
         SDL_QuitSubSystem(SDL_INIT_VIDEO);
         return 1;
      }

      const int maxWidth = std::max(1, static_cast<int>(bounds.w * 0.85));
      const int maxHeight = std::max(1, static_cast<int>(bounds.h * 0.85));
      const int scaleX = std::max(1, maxWidth / imageWidth);
      const int scaleY = std::max(1, maxHeight / imageHeight);
      scale = std::max(1, std::min(scaleX, scaleY));
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return scale;
#else
      return 1;
#endif
   }

#ifdef GNINE_HAVE_SDL2
   double clampPreviewChannel(double value)
   {
      if (value < 0.0)
         return 0.0;
      if (value > 1.0)
         return 1.0;
      return value;
   }

   class PreviewWindow
   {
   public:
      PreviewWindow()
         : _initialized(false), _window(NULL), _renderer(NULL), _texture(NULL),
           _imageWidth(0), _imageHeight(0), _windowWidth(0), _windowHeight(0),
           _windowId(0)
      {
      }

      ~PreviewWindow()
      {
         if (_texture != NULL)
            SDL_DestroyTexture(_texture);
         if (_renderer != NULL)
            SDL_DestroyRenderer(_renderer);
         if (_window != NULL)
            SDL_DestroyWindow(_window);
         if (_initialized)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
      }

      void ensureForImage(const Image &image, int targetWidth, int targetHeight)
      {
         if (!_initialized)
         {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
               throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
            _initialized = true;
         }

         if (_window == NULL)
         {
            _window = SDL_CreateWindow("gnine preview",
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       targetWidth,
                                       targetHeight,
                                       SDL_WINDOW_SHOWN);
            if (_window == NULL)
               throw std::runtime_error(std::string("SDL window creation failed: ") + SDL_GetError());
            _windowId = SDL_GetWindowID(_window);

            // Avoid blocking the preview loop on compositor vsync; the runtime already
            // controls frame pacing, and preview should stay responsive even when the
            // game takes longer to evaluate.
            _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED);
            if (_renderer == NULL)
               _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
            else
               SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
         }

         if (_texture == NULL || _imageWidth != image.width() || _imageHeight != image.height())
         {
            if (_texture != NULL)
               SDL_DestroyTexture(_texture);
            _texture = NULL;
            if (_renderer != NULL)
            {
               _texture = SDL_CreateTexture(_renderer,
                                            SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            image.width(),
                                            image.height());
               if (_texture == NULL)
                  throw std::runtime_error(std::string("SDL texture creation failed: ") + SDL_GetError());
            }
            _imageWidth = image.width();
            _imageHeight = image.height();
            _pixels.resize(static_cast<size_t>(_imageWidth) * static_cast<size_t>(_imageHeight) * 4u);
         }

         if (_windowWidth != targetWidth || _windowHeight != targetHeight)
         {
            SDL_SetWindowSize(_window, targetWidth, targetHeight);
            _windowWidth = targetWidth;
            _windowHeight = targetHeight;
         }
      }

      void pollInput(RuntimeInputState *input)
      {
         input->quitRequested = false;
         input->mouseWheelY = 0.0;
         SDL_Event event;
         while (SDL_PollEvent(&event))
         {
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                (_windowId == 0 || event.window.windowID == _windowId))
            {
               input->quitRequested = true;
            }
            if (event.type == SDL_MOUSEWHEEL)
            {
               input->mouseWheelY += static_cast<double>(event.wheel.y);
            }
         }

         SDL_PumpEvents();
         const Uint8 *keys = SDL_GetKeyboardState(NULL);
         input->keyUp = (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) ? 1.0 : 0.0;
         input->keyDown = (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) ? 1.0 : 0.0;
         input->keyLeft = (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) ? 1.0 : 0.0;
         input->keyRight = (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) ? 1.0 : 0.0;
         input->keyW = keys[SDL_SCANCODE_W] ? 1.0 : 0.0;
         input->keyA = keys[SDL_SCANCODE_A] ? 1.0 : 0.0;
         input->keyS = keys[SDL_SCANCODE_S] ? 1.0 : 0.0;
         input->keyD = keys[SDL_SCANCODE_D] ? 1.0 : 0.0;
         input->keySpace = keys[SDL_SCANCODE_SPACE] ? 1.0 : 0.0;
         input->keyReturn = keys[SDL_SCANCODE_RETURN] ? 1.0 : 0.0;
         input->keyEscape = keys[SDL_SCANCODE_ESCAPE] ? 1.0 : 0.0;
         input->keyShift = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 1.0 : 0.0;
         input->keyCtrl = (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) ? 1.0 : 0.0;
         input->keyTab = keys[SDL_SCANCODE_TAB] ? 1.0 : 0.0;
         input->key0 = keys[SDL_SCANCODE_0] ? 1.0 : 0.0;
         input->key1 = keys[SDL_SCANCODE_1] ? 1.0 : 0.0;
         input->key2 = keys[SDL_SCANCODE_2] ? 1.0 : 0.0;
         input->key3 = keys[SDL_SCANCODE_3] ? 1.0 : 0.0;
         input->key4 = keys[SDL_SCANCODE_4] ? 1.0 : 0.0;
         input->key5 = keys[SDL_SCANCODE_5] ? 1.0 : 0.0;
         input->key6 = keys[SDL_SCANCODE_6] ? 1.0 : 0.0;
         input->key7 = keys[SDL_SCANCODE_7] ? 1.0 : 0.0;
         input->key8 = keys[SDL_SCANCODE_8] ? 1.0 : 0.0;
         input->key9 = keys[SDL_SCANCODE_9] ? 1.0 : 0.0;

         int mouseX = 0;
         int mouseY = 0;
         Uint32 mouseMask = SDL_GetMouseState(&mouseX, &mouseY);
         input->mouseLeft = (mouseMask & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1.0 : 0.0;
         input->mouseRight = (mouseMask & SDL_BUTTON(SDL_BUTTON_RIGHT)) ? 1.0 : 0.0;
         input->mouseX = mouseX;
         input->mouseY = mouseY;
      }

      void present(const Image &image,
                   int targetWidth,
                   int targetHeight,
                   const std::string *overlayText = NULL)
      {
         ensureForImage(image, targetWidth, targetHeight);
         for (int row = 0; row < _imageHeight; ++row)
         {
            for (int col = 0; col < _imageWidth; ++col)
            {
               const double red = clampPreviewChannel(image(row, col, image.channelCount() > 1 ? 0 : 0));
               const double green = clampPreviewChannel(image(row, col, image.channelCount() > 1 ? 1 : 0));
               const double blue = clampPreviewChannel(image(row, col, image.channelCount() > 1 ? 2 : 0));
               const size_t pixelOffset =
                   (static_cast<size_t>(row) * static_cast<size_t>(_imageWidth) + static_cast<size_t>(col)) * 4u;
               _pixels[pixelOffset + 0u] = static_cast<uint8_t>(red * 255.0 + 0.5);
               _pixels[pixelOffset + 1u] = static_cast<uint8_t>(green * 255.0 + 0.5);
               _pixels[pixelOffset + 2u] = static_cast<uint8_t>(blue * 255.0 + 0.5);
               _pixels[pixelOffset + 3u] = 255u;
            }
         }

         if (_renderer != NULL && _texture != NULL)
         {
            SDL_UpdateTexture(_texture, NULL, _pixels.data(), _imageWidth * 4);
            SDL_RenderClear(_renderer);
            SDL_RenderCopy(_renderer, _texture, NULL, NULL);
            if (overlayText != NULL && !overlayText->empty())
               drawOverlayTextRenderer(*overlayText);
            SDL_RenderPresent(_renderer);
            return;
         }

         if (overlayText != NULL && !overlayText->empty())
            drawOverlayText(*overlayText);

         SDL_Surface *surface = SDL_GetWindowSurface(_window);
         if (surface == NULL)
            throw std::runtime_error(std::string("SDL window surface failed: ") + SDL_GetError());
         if (_windowWidth != _imageWidth || _windowHeight != _imageHeight)
            throw std::runtime_error("software preview path does not support scaled window output");
         if (SDL_ConvertPixels(_imageWidth,
                               _imageHeight,
                               SDL_PIXELFORMAT_RGBA32,
                               _pixels.data(),
                               _imageWidth * 4,
                               surface->format->format,
                               surface->pixels,
                               surface->pitch) != 0)
            throw std::runtime_error(std::string("SDL pixel conversion failed: ") + SDL_GetError());
         if (SDL_UpdateWindowSurface(_window) != 0)
            throw std::runtime_error(std::string("SDL window surface update failed: ") + SDL_GetError());
      }

      void setTitle(const std::string &title)
      {
         if (_window != NULL)
            SDL_SetWindowTitle(_window, title.c_str());
      }

   private:
      static const char *const *glyphRows(char ch)
      {
         static const char *const glyph0[7] = {
             "01110",
             "10001",
             "10011",
             "10101",
             "11001",
             "10001",
             "01110"};
         static const char *const glyph1[7] = {
             "00100",
             "01100",
             "00100",
             "00100",
             "00100",
             "00100",
             "01110"};
         static const char *const glyph2[7] = {
             "01110",
             "10001",
             "00001",
             "00010",
             "00100",
             "01000",
             "11111"};
         static const char *const glyph3[7] = {
             "11110",
             "00001",
             "00001",
             "01110",
             "00001",
             "00001",
             "11110"};
         static const char *const glyph4[7] = {
             "00010",
             "00110",
             "01010",
             "10010",
             "11111",
             "00010",
             "00010"};
         static const char *const glyph5[7] = {
             "11111",
             "10000",
             "10000",
             "11110",
             "00001",
             "00001",
             "11110"};
         static const char *const glyph6[7] = {
             "01110",
             "10000",
             "10000",
             "11110",
             "10001",
             "10001",
             "01110"};
         static const char *const glyph7[7] = {
             "11111",
             "00001",
             "00010",
             "00100",
             "01000",
             "01000",
             "01000"};
         static const char *const glyph8[7] = {
             "01110",
             "10001",
             "10001",
             "01110",
             "10001",
             "10001",
             "01110"};
         static const char *const glyph9[7] = {
             "01110",
             "10001",
             "10001",
             "01111",
             "00001",
             "00001",
             "01110"};
         static const char *const glyphDot[7] = {
             "00000",
             "00000",
             "00000",
             "00000",
             "00000",
             "00110",
             "00110"};
         static const char *const glyphDash[7] = {
             "00000",
             "00000",
             "00000",
             "01110",
             "00000",
             "00000",
             "00000"};
         static const char *const glyphSpace[7] = {
             "00000",
             "00000",
             "00000",
             "00000",
             "00000",
             "00000",
             "00000"};
         static const char *const glyphF[7] = {
             "11111",
             "10000",
             "10000",
             "11110",
             "10000",
             "10000",
             "10000"};
         static const char *const glyphI[7] = {
             "11111",
             "00100",
             "00100",
             "00100",
             "00100",
             "00100",
             "11111"};
         static const char *const glyphJ[7] = {
             "00111",
             "00010",
             "00010",
             "00010",
             "00010",
             "10010",
             "01100"};
         static const char *const glyphC[7] = {
             "01111",
             "10000",
             "10000",
             "10000",
             "10000",
             "10000",
             "01111"};
         static const char *const glyphA[7] = {
             "01110",
             "10001",
             "10001",
             "11111",
             "10001",
             "10001",
             "10001"};
         static const char *const glyphL[7] = {
             "10000",
             "10000",
             "10000",
             "10000",
             "10000",
             "10000",
             "11111"};
         static const char *const glyphP[7] = {
             "11110",
             "10001",
             "10001",
             "11110",
             "10000",
             "10000",
             "10000"};
         static const char *const glyphS[7] = {
             "01111",
             "10000",
             "10000",
             "01110",
             "00001",
             "00001",
             "11110"};
         static const char *const glyphT[7] = {
             "11111",
             "00100",
             "00100",
             "00100",
             "00100",
             "00100",
             "00100"};
         static const char *const glyphU[7] = {
             "10001",
             "10001",
             "10001",
             "10001",
             "10001",
             "10001",
             "01110"};

         switch (ch)
         {
         case 'A':
            return glyphA;
         case 'C':
            return glyphC;
         case '0':
            return glyph0;
         case '1':
            return glyph1;
         case '2':
            return glyph2;
         case '3':
            return glyph3;
         case '4':
            return glyph4;
         case '5':
            return glyph5;
         case '6':
            return glyph6;
         case '7':
            return glyph7;
         case '8':
            return glyph8;
         case '9':
            return glyph9;
         case '.':
            return glyphDot;
         case '-':
            return glyphDash;
         case ' ':
            return glyphSpace;
         case 'F':
            return glyphF;
         case 'I':
            return glyphI;
         case 'J':
            return glyphJ;
         case 'L':
            return glyphL;
         case 'P':
            return glyphP;
         case 'S':
            return glyphS;
         case 'T':
            return glyphT;
         case 'U':
            return glyphU;
         default:
            return glyphSpace;
         }
      }

      void blendPixel(int x, int y, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
      {
         if (x < 0 || y < 0 || x >= _imageWidth || y >= _imageHeight)
            return;

         const size_t pixelOffset =
             (static_cast<size_t>(y) * static_cast<size_t>(_imageWidth) + static_cast<size_t>(x)) * 4u;
         const int invAlpha = 255 - alpha;
         _pixels[pixelOffset + 0u] =
             static_cast<uint8_t>((static_cast<int>(_pixels[pixelOffset + 0u]) * invAlpha + red * alpha) / 255);
         _pixels[pixelOffset + 1u] =
             static_cast<uint8_t>((static_cast<int>(_pixels[pixelOffset + 1u]) * invAlpha + green * alpha) / 255);
         _pixels[pixelOffset + 2u] =
             static_cast<uint8_t>((static_cast<int>(_pixels[pixelOffset + 2u]) * invAlpha + blue * alpha) / 255);
         _pixels[pixelOffset + 3u] = 255u;
      }

      void fillRect(int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
      {
         for (int row = 0; row < height; ++row)
         {
            for (int col = 0; col < width; ++col)
               blendPixel(x + col, y + row, red, green, blue, alpha);
         }
      }

      void drawGlyph(int x, int y, int scale, char ch, uint8_t red, uint8_t green, uint8_t blue)
      {
         const char *const *rows = glyphRows(ch);
         for (int row = 0; row < 7; ++row)
         {
            for (int col = 0; col < 5; ++col)
            {
               if (rows[row][col] != '1')
                  continue;
               fillRect(x + col * scale, y + row * scale, scale, scale, red, green, blue, 255);
            }
         }
      }

      void drawText(int x, int y, int scale, const std::string &text, uint8_t red, uint8_t green, uint8_t blue)
      {
         const int advance = 6 * scale;
         for (size_t idx = 0; idx < text.size(); ++idx)
            drawGlyph(x + static_cast<int>(idx) * advance, y, scale, text[idx], red, green, blue);
      }

      void drawOverlayText(const std::string &text)
      {
         const int scale = std::max(1, std::min(3, std::min(_imageWidth, _imageHeight) / 120));
         const int padding = 2 * scale;
         const int margin = 4 * scale;
         const int textWidth = static_cast<int>(text.size()) * 6 * scale - scale;
         const int textHeight = 7 * scale;
         const int boxWidth = textWidth + padding * 2;
         const int boxHeight = textHeight + padding * 2;

         fillRect(margin, margin, boxWidth, boxHeight, 0, 0, 0, 160);
         drawText(margin + padding, margin + padding, scale, text, 255, 255, 255);
      }

      void fillRectRenderer(int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
      {
         SDL_Rect rect;
         rect.x = x;
         rect.y = y;
         rect.w = width;
         rect.h = height;
         SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);
         SDL_SetRenderDrawColor(_renderer, red, green, blue, alpha);
         SDL_RenderFillRect(_renderer, &rect);
      }

      void drawGlyphRenderer(int x, int y, int scale, char ch, uint8_t red, uint8_t green, uint8_t blue)
      {
         const char *const *rows = glyphRows(ch);
         for (int row = 0; row < 7; ++row)
         {
            for (int col = 0; col < 5; ++col)
            {
               if (rows[row][col] != '1')
                  continue;
               fillRectRenderer(x + col * scale, y + row * scale, scale, scale, red, green, blue, 255);
            }
         }
      }

      void drawTextRenderer(int x, int y, int scale, const std::string &text, uint8_t red, uint8_t green, uint8_t blue)
      {
         const int advance = 6 * scale;
         for (size_t idx = 0; idx < text.size(); ++idx)
            drawGlyphRenderer(x + static_cast<int>(idx) * advance, y, scale, text[idx], red, green, blue);
      }

      void drawOverlayTextRenderer(const std::string &text)
      {
         const int scale = std::min(_windowWidth, _windowHeight) < 240 ? 1 : 2;
         const int padding = 2 * scale;
         const int margin = 4 * scale;
         const int textWidth = static_cast<int>(text.size()) * 6 * scale - scale;
         const int textHeight = 7 * scale;
         const int boxWidth = textWidth + padding * 2;
         const int boxHeight = textHeight + padding * 2;

         fillRectRenderer(margin, margin, boxWidth, boxHeight, 0, 0, 0, 160);
         drawTextRenderer(margin + padding, margin + padding, scale, text, 255, 255, 255);
      }

      bool _initialized;
      SDL_Window *_window;
      SDL_Renderer *_renderer;
      SDL_Texture *_texture;
      int _imageWidth;
      int _imageHeight;
      int _windowWidth;
      int _windowHeight;
      Uint32 _windowId;
      std::vector<uint8_t> _pixels;
   };
#endif

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

   double parsePositiveDouble(const std::string &text, const std::string &flagName)
   {
      std::stringstream ss(text);
      double value = 0.0;
      char trailing = '\0';
      ss >> value;
      if (!ss || (ss >> trailing) || value <= 0.0)
         throw std::runtime_error(flagName + " expects a positive number");
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
      if (program.usesVectorFeatures)
      {
         size_t inputCount = 0;
         for (size_t idx = 0; idx < program.argBindings.size(); ++idx)
            inputCount = std::max(inputCount, program.argBindings[idx].inputIndex + 1);
         return inputCount;
      }

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
      std::ostringstream suffix;
      suffix << "_" << std::setw(4) << std::setfill('0') << iteration;

      size_t slashPos = basePath.find_last_of("/\\");
      size_t dotPos = basePath.find_last_of('.');
      if (dotPos == std::string::npos || (slashPos != std::string::npos && dotPos < slashPos))
         return basePath + suffix.str();

      return basePath.substr(0, dotPos) + suffix.str() + basePath.substr(dotPos);
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
                            std::vector<double *> &channelPtrs,
                            std::vector<int32_t> &inputWidths,
                            std::vector<int32_t> &inputHeights,
                            std::vector<int32_t> &inputStrides)
   {
      channelPtrs.clear();
      channelPtrs.reserve(images.size());
      inputWidths.clear();
      inputWidths.reserve(images.size());
      inputHeights.clear();
      inputHeights.reserve(images.size());
      inputStrides.clear();
      inputStrides.reserve(images.size());
      for (const Image &image : images)
      {
         int sourceChannel = image.channelCount() == 1 ? 0 : channel;
         channelPtrs.push_back(const_cast<double *>(image.getChannelData(sourceChannel)));
         inputWidths.push_back(image.width());
         inputHeights.push_back(image.height());
         inputStrides.push_back(image.stride());
      }
   }

   void fillVectorArgPointers(const std::vector<Image> &images,
                              const std::vector<VectorArgBinding> &bindings,
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
         const VectorArgBinding &binding = bindings[idx];
         const Image &image = images[binding.inputIndex];
         int sourceChannel = image.channelCount() == 1 ? 0 : binding.channel;
         dataPtrs.push_back(const_cast<double *>(image.getChannelData(sourceChannel)));
         inputWidths.push_back(image.width());
         inputHeights.push_back(image.height());
         inputStrides.push_back(image.stride());
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

   struct RuntimePreludeProgram
   {
      Cell argsCell;
      std::vector<Cell> defineForms;
      const Cell *bodyExpr;

      RuntimePreludeProgram()
         : bodyExpr(NULL)
      {
      }
   };

   bool isRuntimeDefineForm(const Cell &expr)
   {
      return expr.type == Cell::List &&
             expr.list.size() == 3 &&
             expr.list[0].type == Cell::Symbol &&
             expr.list[0].val == "define" &&
             expr.list[1].type == Cell::Symbol;
   }

   bool parseRuntimePreludeProgram(const Cell &program, RuntimePreludeProgram *outProgram)
   {
      if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
         return false;

      outProgram->argsCell = program.list[0];
      outProgram->defineForms.clear();
      outProgram->bodyExpr = NULL;

      bool sawBody = false;
      for (size_t idx = 1; idx < program.list.size(); ++idx)
      {
         const Cell &expr = program.list[idx];
         if (isRuntimeDefineForm(expr))
         {
            if (sawBody)
               throw std::runtime_error("Runtime prelude defines must appear before the body expression");
            outProgram->defineForms.push_back(expr);
            continue;
         }

         if (sawBody)
            throw std::runtime_error("Runtime program must contain a single body expression");

         outProgram->bodyExpr = &expr;
         sawBody = true;
      }

      if (!sawBody)
         throw std::runtime_error("Runtime program must contain a body expression");

      return true;
   }

   Cell buildRuntimePreludeProgram(const RuntimePreludeProgram &program)
   {
      Cell prelude(Cell::List);
      prelude.list.push_back(program.argsCell);
      for (size_t idx = 0; idx < program.defineForms.size(); ++idx)
         prelude.list.push_back(program.defineForms[idx]);
      prelude.list.push_back(Cell(Cell::Number, "0"));
      return prelude;
   }

   Cell buildRuntimeBodyProgram(const RuntimePreludeProgram &program)
   {
      Cell body(Cell::List);
      body.list.push_back(program.argsCell);
      body.list.push_back(*program.bodyExpr);
      return body;
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

   void rootRuntimeBindings(runtime::Evaluator &evaluator,
                            const std::map<std::string, runtime::Value> &bindings,
                            std::vector<runtime::Value> *rootedBindings)
   {
      rootedBindings->clear();
      rootedBindings->reserve(bindings.size());
      for (std::map<std::string, runtime::Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
         rootedBindings->push_back(it->second);
      for (size_t idx = 0; idx < rootedBindings->size(); ++idx)
         evaluator.heap().addRoot(&(*rootedBindings)[idx]);
   }

   struct RootedRuntimeBindingSet
   {
      runtime::Evaluator *evaluator;
      std::vector<runtime::Value> values;

      RootedRuntimeBindingSet()
         : evaluator(NULL)
      {
      }

      explicit RootedRuntimeBindingSet(runtime::Evaluator &eval)
         : evaluator(&eval)
      {
      }

      ~RootedRuntimeBindingSet()
      {
         clear();
      }

      void clear()
      {
         if (evaluator != NULL)
         {
            for (size_t idx = 0; idx < values.size(); ++idx)
               evaluator->heap().removeRoot(&values[idx]);
         }
         values.clear();
      }

      void set(runtime::Evaluator &eval, const std::map<std::string, runtime::Value> &bindings)
      {
         clear();
         evaluator = &eval;
         rootRuntimeBindings(eval, bindings, &values);
      }
   };

   void addRuntimeImageBindings(runtime::Evaluator &evaluator,
                                const Cell &argsCell,
                                const std::vector<Image> &inputImages,
                                std::map<std::string, runtime::Value> *bindings)
   {
      if (argsCell.list.size() != inputImages.size())
         throw std::runtime_error("Runtime input count does not match program arguments");

      for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         (*bindings)[runtimeArgumentBindingName(argsCell.list[idx], idx)] = evaluator.imageValue(inputImages[idx]);
   }

   double parseTraceMetric(const std::string &traceEntry, const std::string &metricName)
   {
      const std::string marker = metricName + "=";
      size_t pos = traceEntry.find(marker);
      if (pos == std::string::npos)
         return 0.0;

      pos += marker.size();
      size_t end = pos;
      while (end < traceEntry.size() &&
             (isdigit(static_cast<unsigned char>(traceEntry[end])) != 0 || traceEntry[end] == '.'))
         ++end;

      std::stringstream ss(traceEntry.substr(pos, end - pos));
      double value = 0.0;
      ss >> value;
      return ss ? value : 0.0;
   }

   struct RuntimeTraceMetrics
   {
      double compileMillis;
      double executeMillis;
      int executeCalls;
   };

   RuntimeTraceMetrics collectRuntimeTraceMetrics(const std::vector<std::string> &trace)
   {
      RuntimeTraceMetrics metrics;
      metrics.compileMillis = 0.0;
      metrics.executeMillis = 0.0;
      metrics.executeCalls = 0;
      for (const std::string &entry : trace)
      {
         metrics.compileMillis += parseTraceMetric(entry, "compile_ms");
         if (entry.find("execute_ms=") != std::string::npos)
         {
            metrics.executeMillis += parseTraceMetric(entry, "execute_ms");
            ++metrics.executeCalls;
         }
      }
      return metrics;
   }

   const Image *tryGetRuntimeImage(const runtime::Value &value)
   {
      if (value.isObject() && value.object->type == runtime::Object::Image)
      {
         runtime::ImageObject *imageObj = static_cast<runtime::ImageObject *>(value.object);
         return imageObj->image;
      }

      if (value.isObject() && value.object->type == runtime::Object::Tuple)
      {
         runtime::TupleObject *tupleObj = static_cast<runtime::TupleObject *>(value.object);
         if (!tupleObj->values.empty())
            return tryGetRuntimeImage(tupleObj->values[0]);
      }

      return NULL;
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
      std::cout << "--benchmark-no-write skips writing benchmark output images.\n";
      std::cout << "--benchmark-repeats=N reruns runtime benchmarks in one process to expose warm-cache timings, including chained runtime games.\n";
      std::cout << "--emit-frames=PATH writes chained iterations as PATH with _N suffixes.\n";
      std::cout << "--preview opens a live runtime preview window.\n";
      std::cout << "--preview-playback=snake|pong|flappy|brick-breaker runs scripted runtime preview playback without live input.\n";
      std::cout << "--preview-duration-ms=N sets the scripted playback duration in milliseconds.\n";
      std::cout << "--compare[=PATH] writes a side-by-side original/result comparison image.\n";
      std::cout << "--display-scale=N|auto writes output frames enlarged by an integer factor or fits them to the current display.\n";
      std::cout << "--display-size=WIDTHxHEIGHT writes output frames at an exact size.\n";
      std::cout << "--runtime interprets managed image programs instead of compiling JIT kernels.\n";
      std::cout << "Color images are processed channel-wise and preserve RGB output.\n";
      std::cout << "Top-level form (iterate N ((A) ...)) runs the full transform N times.\n";
      std::cout << "Top-level form (iterate-state N ((A ...) init) ((state) ...)) seeds runtime chaining from an explicit initial state.\n";
      std::cout << "Top-level form (iterate-until N ((A ...) init) ((state) step) ((state) done)) runs until done is nonzero or N steps are reached.\n";
      std::cout << "Inside a transform, iter is the 1-based chained iteration counter.\n";
      std::cout << "Runtime form (canvas W H [C] expr) builds a new image by evaluating expr for each output pixel under i/j/c.\n";
      std::cout << "Runtime builtins (draw-rect cx cy half_w half_h value) and (draw-circle cx cy radius value) return value inside shape, 0 outside.\n";
      std::cout << "Runtime preview/input bindings include key-up/key-down/key-left/key-right, key-w/key-a/key-s/key-d, key-space, key-return, key-escape, mouse-x, mouse-y, mouse-left, mouse-right, preview-time-ms, and preview-delta-ms.\n";
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
   bool benchmarkNoWrite = false;
   bool preview = false;
   bool previewPlayback = false;
   bool runtimeMode = false;
   bool traceFallback = false;
   PreviewPlaybackScenario previewPlaybackScenario = PreviewPlaybackScenario::None;
   double previewPlaybackDurationMs = 10000.0;
   std::string emitFramesPath;
   std::string comparePath;
   int displayScale = 1;
   bool displayScaleAuto = false;
   int displayWidth = 0;
   int displayHeight = 0;
   int n_times = 1;
   int chain_times = 0;
   int benchmarkRepeats = 1;
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
      else if (s == "--benchmark-no-write")
         benchmarkNoWrite = true;
      else if (s == "--preview")
         preview = true;
      else if (s.length() > 19 && s.substr(0, 19) == "--preview-playback=")
      {
         std::string value = s.substr(19);
         if (!parsePreviewPlaybackScenario(value, &previewPlaybackScenario))
            throw std::runtime_error("--preview-playback expects snake, pong, flappy, or brick-breaker");
         previewPlayback = true;
      }
      else if (s.length() > 22 && s.substr(0, 22) == "--preview-duration-ms=")
         previewPlaybackDurationMs = parsePositiveDouble(s.substr(22), "--preview-duration-ms");
      else if (s == "--trace-fallback")
         traceFallback = true;
      else if (s == "--compare")
         comparePath = "__AUTO__";
      else if (s.length() > 10 && s.substr(0, 10) == "--compare=")
         comparePath = s.substr(10);
      else if (s.length() > 16 && s.substr(0, 16) == "--display-scale=")
      {
         std::string value = s.substr(16);
         if (value == "auto")
            displayScaleAuto = true;
         else
            displayScale = parsePositiveInt(value, "--display-scale");
      }
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
      else if (s.length() > 20 && s.substr(0, 20) == "--benchmark-repeats=")
         benchmarkRepeats = parsePositiveInt(s.substr(20), "--benchmark-repeats");
      else
      {
         std::cerr << "Unrecognised command line switch: " << s << std::endl;
         return 1;
      }
   }

   if (chain_times > 0)
      n_times = chain_times;

   if (displayWidth > 0 && (displayScale != 1 || displayScaleAuto))
      throw std::runtime_error("Use either --display-scale or --display-size, not both");
#ifndef GNINE_HAVE_SDL2
   if (displayScaleAuto)
      throw std::runtime_error("--display-scale=auto requires SDL2 support");
#endif
   if (preview && !runtimeMode)
      throw std::runtime_error("--preview currently requires --runtime");
   if (preview && benchmark)
      throw std::runtime_error("--preview does not support --benchmark");
   if (previewPlayback && !runtimeMode)
      throw std::runtime_error("--preview-playback currently requires --runtime");
   if (previewPlayback && preview)
      throw std::runtime_error("--preview-playback cannot be combined with --preview");
   if (previewPlayback && benchmark)
      throw std::runtime_error("--preview-playback does not support --benchmark");
   if (benchmarkRepeats > 1 && !runtimeMode)
      throw std::runtime_error("--benchmark-repeats currently requires --runtime");
   if (benchmarkRepeats > 1 && !emitFramesPath.empty())
      throw std::runtime_error("--benchmark-repeats does not support --emit-frames");

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
   RuntimePreludeProgram runtimePreludeProgram;
   bool hasRuntimePreludeProgram = parseRuntimePreludeProgram(code, &runtimePreludeProgram);
   Cell effectiveCode = hasRuntimePreludeProgram ? buildRuntimeBodyProgram(runtimePreludeProgram) : code;
   Cell iterateStateInit;
   Cell iterateUntilDone;
   bool hasIterateState = false;
   bool hasIterateUntil = false;
   int iterateCount = 0;
   const Cell *iterateProgram = hasRuntimePreludeProgram ? runtimePreludeProgram.bodyExpr : &code;
   if (isTopLevelIterate(*iterateProgram))
   {
      iterateCount = parseIterateCount(*iterateProgram);
      effectiveCode = iterateProgram->list[2];
      if (chain_times == 0)
      {
         chain_times = iterateCount;
         n_times = chain_times;
      }
   }
   else if (isTopLevelIterateState(*iterateProgram))
   {
      iterateCount = parseIterateCount(*iterateProgram);
      iterateStateInit = iterateProgram->list[2];
      effectiveCode = iterateProgram->list[3];
      hasIterateState = true;
      if (chain_times == 0)
      {
         chain_times = iterateCount;
         n_times = chain_times;
      }
   }
   else if (isTopLevelIterateUntil(*iterateProgram))
   {
      iterateCount = parseIterateCount(*iterateProgram);
      iterateStateInit = iterateProgram->list[2];
      effectiveCode = iterateProgram->list[3];
      iterateUntilDone = iterateProgram->list[4];
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

   size_t inputCount = runtimeMode
                           ? (hasRuntimePreludeProgram
                                 ? runtimePreludeProgram.argsCell.list.size()
                                 : programArgumentCount(hasIterateState ? iterateStateInit : effectiveCode))
                           : 0;
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
      const int resolvedDisplayScale = displayScaleAuto
                                           ? autoDisplayScaleForImage(imageToWrite.width(), imageToWrite.height())
                                           : displayScale;
      if (displayWidth > 0)
      {
         Image scaled = resizeNearest(imageToWrite, displayWidth, displayHeight);
         scaled.write(path);
         return;
      }

      if (resolvedDisplayScale != 1)
      {
         Image scaled = resizeNearest(imageToWrite,
                                      imageToWrite.width() * resolvedDisplayScale,
                                      imageToWrite.height() * resolvedDisplayScale);
         scaled.write(path);
         return;
      }

      imageToWrite.write(path);
   };

   auto makeDisplayImage = [&](const Image &imageToWrite) -> Image
   {
      const int resolvedDisplayScale = displayScaleAuto
                                           ? autoDisplayScaleForImage(imageToWrite.width(), imageToWrite.height())
                                           : displayScale;
      if (displayWidth > 0)
         return resizeNearest(imageToWrite, displayWidth, displayHeight);
      if (resolvedDisplayScale != 1)
         return resizeNearest(imageToWrite,
                              imageToWrite.width() * resolvedDisplayScale,
                              imageToWrite.height() * resolvedDisplayScale);
      return copyImage(imageToWrite);
   };

   if (runtimeMode)
   {
      const Image *runtimeImageResult = NULL;
      bool hasImageResult = false;
      runtime::Evaluator evaluator;
      runtime::Value runtimeState = runtime::Value::nil();
      runtime::Heap::Root runtimeStateRoot(evaluator.heap(), runtimeState);
      bool hasRuntimeState = false;
      bool hasScalarResult = false;
      double runtimeScalarResult = 0.0;
      int runtimeIterationsExecuted = 0;
      double runtimeBenchmarkCompileMillis = 0.0;
      double runtimeBenchmarkExecuteMillis = 0.0;
      double runtimeBenchmarkFirstCompileMillis = 0.0;
      double runtimeBenchmarkFirstExecuteMillis = 0.0;
      double runtimeBenchmarkLastCompileMillis = 0.0;
      double runtimeBenchmarkLastExecuteMillis = 0.0;
      int runtimeBenchmarkFirstIterations = 0;
      int runtimeBenchmarkLastIterations = 0;
      int runtimeBenchmarkIterations = 0;
      RuntimeInputState runtimeInputState;
      std::map<std::string, runtime::Value> sharedRuntimeBindings;
      RootedRuntimeBindingSet sharedRuntimeBindingRoots;

      if (!runtimePreludeProgram.defineForms.empty())
      {
         std::map<std::string, runtime::Value> preludeBindings;
         addRuntimeImageBindings(evaluator, runtimePreludeProgram.argsCell, inputImages, &preludeBindings);
         preludeBindings["iter"] = runtime::Value::numberValue(0.0);
         addRuntimeInputBindings(&preludeBindings, runtimeInputState, 0.0, 0.0);
         evaluator.evaluateProgram(buildRuntimePreludeProgram(runtimePreludeProgram), preludeBindings, &sharedRuntimeBindings);
         sharedRuntimeBindingRoots.set(evaluator, sharedRuntimeBindings);
      }

      auto buildRuntimeBindings = [&](const Cell &argsCell,
                                      double iterValue,
                                      const RuntimeInputState &inputState,
                                      double previewTimeMs,
                                      double previewDeltaMs) {
         std::map<std::string, runtime::Value> bindings = sharedRuntimeBindings;
         addRuntimeImageBindings(evaluator, argsCell, inputImages, &bindings);
         bindings["iter"] = runtime::Value::numberValue(iterValue);
         addRuntimeInputBindings(&bindings, inputState, previewTimeMs, previewDeltaMs);
         return bindings;
      };

      auto runSingleRuntimePass = [&](double iterValue,
                                     const RuntimeInputState &inputState,
                                     double previewTimeMs,
                                     double previewDeltaMs) {
         const Cell &argsCell = effectiveCode.list[0];

         evaluator.clearExecutionTrace();
         std::map<std::string, runtime::Value> bindings =
             buildRuntimeBindings(argsCell, iterValue, inputState, previewTimeMs, previewDeltaMs);

         auto passStart = std::chrono::steady_clock::now();
         runtime::Value passState = runRuntimeProgram(evaluator, effectiveCode, bindings);
         auto passEnd = std::chrono::steady_clock::now();
         RuntimeTraceMetrics traceMetrics = collectRuntimeTraceMetrics(evaluator.executionTrace());
         double passExecuteMillis =
             std::chrono::duration_cast<std::chrono::microseconds>(passEnd - passStart).count() / 1000.0;

         if (traceFallback)
         {
            const std::vector<std::string> &trace = evaluator.executionTrace();
            for (const std::string &entry : trace)
            {
               if (entry.find("compiled_fallback=") != std::string::npos ||
                   (entry.find("mode=interpreted") != std::string::npos &&
                    entry.find("fallback=") != std::string::npos))
                  std::cerr << "[trace-fallback] " << entry << std::endl;
            }
         }

         return std::make_pair(std::move(passState),
                               std::make_pair(traceMetrics.compileMillis, passExecuteMillis));
      };

      if (preview)
      {
#ifndef GNINE_HAVE_SDL2
         throw std::runtime_error("--preview requested but this build does not have SDL2 support");
#else
         PreviewWindow previewWindow;
         bool statefulPreview = hasIterateState || hasIterateUntil || chain_times > 0;
         int previewFrame = 0;
         double previewFpsAccumMs = 0.0;
         int previewFpsAccumFrames = 0;
         double previewCpuAccumMs = 0.0;
         int previewRuntimeCallAccumCount = 0;
         double previewDisplayedFps = 0.0;
         double previewDisplayedCpuMs = 0.0;
         double previewDisplayedRuntimeCallUs = 0.0;
         std::string previewOverlayText("FPS -.-- CPU -.-- CALLUS -.--");
         std::chrono::steady_clock::time_point previewStart = std::chrono::steady_clock::now();
         std::chrono::steady_clock::time_point previewLast = previewStart;
         const int previewTargetWidth = displayWidth > 0 ? displayWidth : 0;
         const int previewTargetHeight = displayHeight > 0 ? displayHeight : 0;
         int previewResolvedDisplayScale = displayScaleAuto ? 0 : displayScale;

         if (statefulPreview && effectiveCode.list[0].list.size() != 1)
            throw std::runtime_error("preview runtime chaining requires the step program to take exactly one state argument");
         if (hasIterateUntil && iterateUntilDone.list[0].list.size() != 1)
            throw std::runtime_error("iterate-until done program must take exactly one state argument");

         if (hasIterateState || hasIterateUntil)
         {
            const Cell &initArgsCell = iterateStateInit.list[0];
            std::map<std::string, runtime::Value> initBindings = sharedRuntimeBindings;
            addRuntimeImageBindings(evaluator, initArgsCell, inputImages, &initBindings);
            initBindings["iter"] = runtime::Value::numberValue(0.0);
            addRuntimeInputBindings(&initBindings, runtimeInputState, 0.0, 0.0);
            runtimeState = runRuntimeProgram(evaluator, iterateStateInit, initBindings);
            hasRuntimeState = true;
         }
         else if (statefulPreview)
         {
            if (inputImages.size() != 1)
               throw std::runtime_error("--preview runtime chaining requires a single input image unless you use iterate-state");
            runtimeState = evaluator.imageValue(inputImages[0]);
            hasRuntimeState = true;
         }

         while (true)
         {
            std::chrono::steady_clock::time_point frameNow = std::chrono::steady_clock::now();
            double previewTimeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(frameNow - previewStart).count() / 1000.0;
            double previewDeltaMs =
                std::chrono::duration_cast<std::chrono::microseconds>(frameNow - previewLast).count() / 1000.0;
            previewLast = frameNow;

            previewWindow.pollInput(&runtimeInputState);
            if (runtimeInputState.quitRequested || runtimeInputState.keyEscape != 0.0)
               break;

            ++previewFrame;
            double previewFrameExecuteMs = 0.0;
            int previewFrameRuntimeCallCount = 0;
            if (statefulPreview)
            {
               evaluator.clearExecutionTrace();
               std::map<std::string, runtime::Value> bindings = sharedRuntimeBindings;
               bindings[runtimeArgumentBindingName(effectiveCode.list[0].list[0], 0)] = runtimeState;
               bindings["iter"] = runtime::Value::numberValue(static_cast<double>(previewFrame));
               addRuntimeInputBindings(&bindings, runtimeInputState, previewTimeMs, previewDeltaMs);

               auto passStart = std::chrono::steady_clock::now();
               runtimeState = runRuntimeProgram(evaluator, effectiveCode, bindings);
               auto passEnd = std::chrono::steady_clock::now();
               hasRuntimeState = true;
               runtimeIterationsExecuted = previewFrame;
               RuntimeTraceMetrics traceMetrics = collectRuntimeTraceMetrics(evaluator.executionTrace());
               previewFrameRuntimeCallCount = traceMetrics.executeCalls;
               previewFrameExecuteMs =
                   std::chrono::duration_cast<std::chrono::microseconds>(passEnd - passStart).count() / 1000.0;
               runtimeBenchmarkCompileMillis += traceMetrics.compileMillis;
               runtimeBenchmarkExecuteMillis += previewFrameExecuteMs;
            }
            else
            {
               std::pair<runtime::Value, std::pair<double, double> > passResult =
                   runSingleRuntimePass(static_cast<double>(previewFrame),
                                        runtimeInputState,
                                        previewTimeMs,
                                        previewDeltaMs);
               runtimeState = passResult.first;
               previewFrameExecuteMs = passResult.second.second;
               RuntimeTraceMetrics traceMetrics = collectRuntimeTraceMetrics(evaluator.executionTrace());
               previewFrameRuntimeCallCount = traceMetrics.executeCalls;
               hasRuntimeState = true;
               runtimeBenchmarkCompileMillis += passResult.second.first;
               runtimeBenchmarkExecuteMillis += passResult.second.second;
            }

            const Image *previewImage = tryGetRuntimeImage(runtimeState);
            if (!previewImage)
               throw std::runtime_error("--preview requires the runtime program to return an image or a tuple whose first element is an image");

            runtimeImageResult = previewImage;
            hasImageResult = true;
            hasScalarResult = false;

            if (displayScaleAuto && previewResolvedDisplayScale <= 0)
               previewResolvedDisplayScale = autoDisplayScaleForImage(previewImage->width(), previewImage->height());
            const int resolvedDisplayScale = previewResolvedDisplayScale > 0 ? previewResolvedDisplayScale : 1;
            int targetWidth = previewTargetWidth > 0 ? previewTargetWidth : previewImage->width() * resolvedDisplayScale;
            int targetHeight = previewTargetHeight > 0 ? previewTargetHeight : previewImage->height() * resolvedDisplayScale;
            previewWindow.present(*previewImage, targetWidth, targetHeight, &previewOverlayText);

            previewFpsAccumMs += previewDeltaMs;
            previewCpuAccumMs += previewFrameExecuteMs;
            previewRuntimeCallAccumCount += previewFrameRuntimeCallCount;
            ++previewFpsAccumFrames;
            if (previewFpsAccumMs >= 250.0)
            {
               previewDisplayedFps = (previewFpsAccumFrames * 1000.0) / previewFpsAccumMs;
               previewDisplayedCpuMs = previewCpuAccumMs / previewFpsAccumFrames;
               previewDisplayedRuntimeCallUs =
                   previewRuntimeCallAccumCount > 0 ? ((previewCpuAccumMs * 1000.0) / previewRuntimeCallAccumCount) : 0.0;
               previewFpsAccumMs = 0.0;
               previewCpuAccumMs = 0.0;
               previewRuntimeCallAccumCount = 0;
               previewFpsAccumFrames = 0;

               std::ostringstream overlay;
               overlay << std::fixed << std::setprecision(1)
                       << "FPS " << previewDisplayedFps
                       << " CPU " << std::setprecision(2) << previewDisplayedCpuMs
                       << " CALLUS " << std::setprecision(1) << previewDisplayedRuntimeCallUs;
               previewOverlayText = overlay.str();

               std::ostringstream titleStream;
               titleStream << std::fixed << std::setprecision(2) << previewDisplayedCpuMs;
               previewWindow.setTitle(std::string("gnine preview - ") + previewOverlayText +
                                      " | " + titleStream.str() + " ms/frame");
            }

            if (!emitFramesPath.empty())
               writeDisplayImage(*previewImage, makeChainedFramePath(emitFramesPath, previewFrame));

            if (hasIterateUntil)
            {
               std::map<std::string, runtime::Value> doneBindings = sharedRuntimeBindings;
               doneBindings[runtimeArgumentBindingName(iterateUntilDone.list[0].list[0], 0)] = runtimeState;
               doneBindings["iter"] = runtime::Value::numberValue(static_cast<double>(previewFrame));
               addRuntimeInputBindings(&doneBindings, runtimeInputState, previewTimeMs, previewDeltaMs);
               runtime::Value doneValue = runRuntimeProgram(evaluator, iterateUntilDone, doneBindings);
               if (!doneValue.isNumber())
                  throw std::runtime_error("iterate-until done program must return a number");
               if (doneValue.number != 0.0)
                  break;
            }

            if (chain_times > 0 && !hasIterateUntil && previewFrame >= chain_times)
               break;
         }
#endif
      }
      else if (previewPlayback)
      {
         const double previewFrameDeltaMs = 1000.0 / 60.0;
         const int previewFrameBudget = previewPlaybackFrameBudget(previewPlaybackDurationMs, previewFrameDeltaMs);
         int previewFrame = 0;
         bool statefulPreview = hasIterateState || hasIterateUntil || chain_times > 0;

         if (statefulPreview && effectiveCode.list[0].list.size() != 1)
            throw std::runtime_error("preview playback requires the step program to take exactly one state argument");
         if (hasIterateUntil && iterateUntilDone.list[0].list.size() != 1)
            throw std::runtime_error("iterate-until done program must take exactly one state argument");

         if (hasIterateState || hasIterateUntil)
         {
            const Cell &initArgsCell = iterateStateInit.list[0];
            std::map<std::string, runtime::Value> initBindings = sharedRuntimeBindings;
            addRuntimeImageBindings(evaluator, initArgsCell, inputImages, &initBindings);
            initBindings["iter"] = runtime::Value::numberValue(0.0);
            addRuntimeInputBindings(&initBindings,
                                    makePreviewPlaybackInput(previewPlaybackScenario, 0),
                                    0.0,
                                    0.0);
            runtimeState = runRuntimeProgram(evaluator, iterateStateInit, initBindings);
            hasRuntimeState = true;
         }
         else if (statefulPreview)
         {
            if (inputImages.size() != 1)
               throw std::runtime_error("--preview-playback requires a single input image unless you use iterate-state");
            runtimeState = evaluator.imageValue(inputImages[0]);
            hasRuntimeState = true;
         }

         while (previewFrame < previewFrameBudget)
         {
            RuntimeInputState playbackInput = makePreviewPlaybackInput(previewPlaybackScenario, previewFrame);
            double previewTimeMs = static_cast<double>(previewFrame) * previewFrameDeltaMs;
            double previewDeltaMs = previewFrameDeltaMs;
            ++previewFrame;

            if (statefulPreview)
            {
               evaluator.clearExecutionTrace();
               std::map<std::string, runtime::Value> bindings = sharedRuntimeBindings;
               bindings[runtimeArgumentBindingName(effectiveCode.list[0].list[0], 0)] = runtimeState;
               bindings["iter"] = runtime::Value::numberValue(static_cast<double>(previewFrame));
               addRuntimeInputBindings(&bindings, playbackInput, previewTimeMs, previewDeltaMs);

               auto passStart = std::chrono::steady_clock::now();
               runtimeState = runRuntimeProgram(evaluator, effectiveCode, bindings);
               auto passEnd = std::chrono::steady_clock::now();
               hasRuntimeState = true;
               runtimeIterationsExecuted = previewFrame;
               RuntimeTraceMetrics traceMetrics = collectRuntimeTraceMetrics(evaluator.executionTrace());
               double previewFrameExecuteMs =
                   std::chrono::duration_cast<std::chrono::microseconds>(passEnd - passStart).count() / 1000.0;
               runtimeBenchmarkCompileMillis += traceMetrics.compileMillis;
               runtimeBenchmarkExecuteMillis += previewFrameExecuteMs;
            }
            else
            {
               std::pair<runtime::Value, std::pair<double, double> > passResult =
                   runSingleRuntimePass(static_cast<double>(previewFrame),
                                        playbackInput,
                                        previewTimeMs,
                                        previewDeltaMs);
               runtimeState = passResult.first;
               hasRuntimeState = true;
               runtimeBenchmarkCompileMillis += passResult.second.first;
               runtimeBenchmarkExecuteMillis += passResult.second.second;
            }

            const Image *previewImage = tryGetRuntimeImage(runtimeState);
            if (!previewImage)
               throw std::runtime_error("--preview-playback requires the runtime program to return an image or a tuple whose first element is an image");

            runtimeImageResult = previewImage;
            hasImageResult = true;
            hasScalarResult = false;

            if (!emitFramesPath.empty())
               writeDisplayImage(*previewImage, makeChainedFramePath(emitFramesPath, previewFrame));
         }
      }
      else if (chain_times > 0)
      {
         const int runtimeChainRepeats = benchmark ? benchmarkRepeats : 1;
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

         for (int repeat = 0; repeat < runtimeChainRepeats; ++repeat)
         {
            double repeatCompileMillis = 0.0;
            double repeatExecuteMillis = 0.0;
            int repeatIterationsExecuted = 0;

            if (hasIterateState || hasIterateUntil)
            {
               const Cell &initArgsCell = iterateStateInit.list[0];
               std::map<std::string, runtime::Value> initBindings = sharedRuntimeBindings;
               addRuntimeImageBindings(evaluator, initArgsCell, inputImages, &initBindings);
               initBindings["iter"] = runtime::Value::numberValue(0.0);
               addRuntimeInputBindings(&initBindings, runtimeInputState, 0.0, 0.0);
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

            std::map<std::string, runtime::Value> stepBindings = sharedRuntimeBindings;
            std::map<std::string, runtime::Value> doneBindings = sharedRuntimeBindings;
            const std::string stateBindingName = runtimeArgumentBindingName(stateArgsCell.list[0], 0);
            const std::string doneBindingName = hasIterateUntil
                                                    ? runtimeArgumentBindingName(iterateUntilDone.list[0].list[0], 0)
                                                    : std::string();

            for (int iter = 1; iter <= chain_times; ++iter)
            {
               evaluator.clearExecutionTrace();
               stepBindings[stateBindingName] = runtimeState;
               stepBindings["iter"] = runtime::Value::numberValue(static_cast<double>(iter));
               addRuntimeInputBindings(&stepBindings, runtimeInputState, 0.0, 0.0);
               auto passStart = std::chrono::steady_clock::now();
               runtimeState = runRuntimeProgram(evaluator, effectiveCode, stepBindings);
               auto passEnd = std::chrono::steady_clock::now();
               hasRuntimeState = true;
               runtimeIterationsExecuted = iter;
               repeatIterationsExecuted = iter;
               RuntimeTraceMetrics traceMetrics = collectRuntimeTraceMetrics(evaluator.executionTrace());
               double passExecuteMillis =
                   std::chrono::duration_cast<std::chrono::microseconds>(passEnd - passStart).count() / 1000.0;
               repeatCompileMillis += traceMetrics.compileMillis;
               repeatExecuteMillis += passExecuteMillis;
               runtimeBenchmarkCompileMillis += traceMetrics.compileMillis;
               runtimeBenchmarkExecuteMillis += passExecuteMillis;
               ++runtimeBenchmarkIterations;

               const Image *nextImage = tryGetRuntimeImage(runtimeState);
               if (!nextImage)
                  throw std::runtime_error("Runtime chained execution requires the program to return an image or a tuple whose first element is an image");

               runtimeImageResult = nextImage;
               hasImageResult = true;

               if (!emitFramesPath.empty())
                  writeDisplayImage(*nextImage, makeChainedFramePath(emitFramesPath, iter));

               if (hasIterateUntil)
               {
                  doneBindings[doneBindingName] = runtimeState;
                  doneBindings["iter"] = runtime::Value::numberValue(static_cast<double>(iter));
                  addRuntimeInputBindings(&doneBindings, runtimeInputState, 0.0, 0.0);
                  runtime::Value doneValue = runRuntimeProgram(evaluator, iterateUntilDone, doneBindings);
                  if (!doneValue.isNumber())
                     throw std::runtime_error("iterate-until done program must return a number");
                  if (doneValue.number != 0.0)
                     break;
               }
            }

            if (benchmark && runtimeChainRepeats > 1)
            {
               if (repeat == 0)
               {
                  runtimeBenchmarkFirstCompileMillis = repeatCompileMillis;
                  runtimeBenchmarkFirstExecuteMillis = repeatExecuteMillis;
                  runtimeBenchmarkFirstIterations = repeatIterationsExecuted;
               }
               runtimeBenchmarkLastCompileMillis = repeatCompileMillis;
               runtimeBenchmarkLastExecuteMillis = repeatExecuteMillis;
               runtimeBenchmarkLastIterations = repeatIterationsExecuted;
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

         const int passes = benchmark ? benchmarkRepeats : 1;
         for (int pass = 0; pass < passes; ++pass)
         {
            std::pair<runtime::Value, std::pair<double, double>> passResult =
                runSingleRuntimePass(static_cast<double>(pass + 1), runtimeInputState, 0.0, 0.0);
            runtimeState = passResult.first;
            hasRuntimeState = true;
            double passCompileMillis = passResult.second.first;
            double passExecuteMillis = passResult.second.second;
            runtimeBenchmarkCompileMillis += passCompileMillis;
            runtimeBenchmarkExecuteMillis += passExecuteMillis;
            if (pass == 0)
            {
               runtimeBenchmarkFirstCompileMillis = passCompileMillis;
               runtimeBenchmarkFirstExecuteMillis = passExecuteMillis;
            }
            runtimeBenchmarkLastCompileMillis = passCompileMillis;
            runtimeBenchmarkLastExecuteMillis = passExecuteMillis;

            const Image *singleResultImage = tryGetRuntimeImage(runtimeState);
            if (singleResultImage)
            {
               runtimeImageResult = singleResultImage;
               hasImageResult = true;
               hasScalarResult = false;
            }
            else if (runtimeState.isNumber())
            {
               hasScalarResult = true;
               hasImageResult = false;
               runtimeScalarResult = runtimeState.number;
            }
         }
      }

      if (hasImageResult)
      {
         if (!runtimeImageResult)
            throw std::runtime_error("Runtime mode expected an image result but did not find one");
         if (!benchmarkNoWrite)
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
         int benchmarkIterations = chain_times > 0 ? runtimeBenchmarkIterations : benchmarkRepeats;
         double averageMillis = benchmarkIterations > 0 ? runtimeBenchmarkExecuteMillis / benchmarkIterations : 0.0;
         std::cout << "benchmark.compile_ms=" << runtimeBenchmarkCompileMillis << std::endl;
         std::cout << "benchmark.execute_ms=" << runtimeBenchmarkExecuteMillis << std::endl;
         std::cout << "benchmark.iterations=" << benchmarkIterations << std::endl;
         std::cout << "benchmark.mode=" << (hasIterateUntil
                                                ? (benchmarkRepeats > 1 ? "runtime-until-repeat" : "runtime-until")
                                                : (chain_times > 0
                                                       ? (benchmarkRepeats > 1 ? "runtime-chain-repeat" : "runtime-chain")
                                                       : (benchmarkRepeats > 1 ? "runtime-repeat" : "runtime"))) << std::endl;
         std::cout << "benchmark.avg_iter_ms=" << averageMillis << std::endl;
         if (benchmarkRepeats > 1)
         {
            if (chain_times > 0)
            {
               double firstRepeatAverageMillis =
                   runtimeBenchmarkFirstIterations > 0 ? runtimeBenchmarkFirstExecuteMillis / runtimeBenchmarkFirstIterations : 0.0;
               double lastRepeatAverageMillis =
                   runtimeBenchmarkLastIterations > 0 ? runtimeBenchmarkLastExecuteMillis / runtimeBenchmarkLastIterations : 0.0;
               std::cout << "benchmark.first_repeat_compile_ms=" << runtimeBenchmarkFirstCompileMillis << std::endl;
               std::cout << "benchmark.first_repeat_execute_ms=" << runtimeBenchmarkFirstExecuteMillis << std::endl;
               std::cout << "benchmark.first_repeat_iterations=" << runtimeBenchmarkFirstIterations << std::endl;
               std::cout << "benchmark.first_repeat_avg_iter_ms=" << firstRepeatAverageMillis << std::endl;
               std::cout << "benchmark.last_repeat_compile_ms=" << runtimeBenchmarkLastCompileMillis << std::endl;
               std::cout << "benchmark.last_repeat_execute_ms=" << runtimeBenchmarkLastExecuteMillis << std::endl;
               std::cout << "benchmark.last_repeat_iterations=" << runtimeBenchmarkLastIterations << std::endl;
               std::cout << "benchmark.last_repeat_avg_iter_ms=" << lastRepeatAverageMillis << std::endl;
            }
            else
            {
               std::cout << "benchmark.first_compile_ms=" << runtimeBenchmarkFirstCompileMillis << std::endl;
               std::cout << "benchmark.first_execute_ms=" << runtimeBenchmarkFirstExecuteMillis << std::endl;
               std::cout << "benchmark.last_compile_ms=" << runtimeBenchmarkLastCompileMillis << std::endl;
               std::cout << "benchmark.last_execute_ms=" << runtimeBenchmarkLastExecuteMillis << std::endl;
            }
         }
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
   std::vector<int32_t> inputWidths;
   std::vector<int32_t> inputHeights;
   std::vector<int32_t> inputStrides;
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
               fillVectorArgPointers(chainImages, loweredProgram.argBindings,
                                     dataPtrs, inputWidths, inputHeights, inputStrides);
            else
            {
               fillChannelPointers(chainImages, channel, dataPtrs, inputWidths, inputHeights, inputStrides);
               dataPtrs[0] = chainInput.getChannelData(channel);
            }

            size_t functionIndex = loweredProgram.usesVectorFeatures ? static_cast<size_t>(channel) : 0;
            compiledFunctions[functionIndex](image->width(),
                                             image->height(),
                                             i + 1,
                                             dataPtrs.data(),
                                             inputWidths.data(),
                                             inputHeights.data(),
                                             inputStrides.data(),
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
               fillVectorArgPointers(inputImages, loweredProgram.argBindings,
                                     dataPtrs, inputWidths, inputHeights, inputStrides);
            else
               fillChannelPointers(inputImages, channel, dataPtrs, inputWidths, inputHeights, inputStrides);

            size_t functionIndex = loweredProgram.usesVectorFeatures ? static_cast<size_t>(channel) : 0;
            compiledFunctions[functionIndex](image->width(),
                                             image->height(),
                                             1,
                                             dataPtrs.data(),
                                             inputWidths.data(),
                                             inputHeights.data(),
                                             inputStrides.data(),
                                             outIm.getChannelData(channel));
         }
      }
   }
   auto executionEnd = std::chrono::steady_clock::now();

   Image writtenOutput = makeDisplayImage(outIm);
   if (!benchmarkNoWrite)
   {
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
