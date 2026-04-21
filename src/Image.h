/*******************************************************************************
 * This originates from Pixslam
 * Original Author is Luke Dodd
 ********************************************************************************/

#pragma once

#include <string>
#include <vector>

namespace gnine
{

   class Image
   {
   private:
      // TODO: think about dealing with different datatypes?
      // The image class now supports grayscale and planar RGB doubles.
      typedef double PixType;

      // PixType *data;
      int w, h;
      int s;
      int channels;
      bool ownsData;

   public:
      PixType *data;

      // Load from png image.
      // File errors result in a 0x0 sized image.
      explicit Image(const std::string &path);

      // Construct new, zeroed, image.
      Image(int w, int h, int channels_ = 1) : w(w), h(h), s(w), channels(channels_), ownsData(true)
      {
         data = new PixType[w * h * channels];
         std::fill(data, data + (w * h * channels), 0.0);
      }

      // Construct new, zeroed, image with specified stride.
      Image(int w_, int h_, int s_, int channels_ = 1) : w(w_), h(h_), s(s_), channels(channels_), ownsData(true)
      {
         data = new PixType[h * s * channels];
         std::fill(data, data + (h * s * channels), 0.0);
      }

      // Construct image from already loaded data.
      // Does not take ownership, i.e. will not clean up data pointer.
      Image(PixType *data, int w, int h, int s, int channels_ = 1)
          : data(data), w(w), h(h), s(s), channels(channels_), ownsData(false)
      {
      }

      // Copy an image with added padding.
      explicit Image(const Image &original, int padx, int pady)
          : w(original.width() + padx * 2), h(original.height() + pady * 2), s(w), channels(original.channelCount()), ownsData(true)
      {
         data = new PixType[w * h * channels];
         std::fill(data, data + (w * h * channels), 0.0);
         for (int channel = 0; channel < channels; ++channel)
         {
            for (int i = 0; i < original.height(); ++i)
            {
               for (int j = 0; j < original.width(); ++j)
               {
                  (*this)(i + pady, j + padx, channel) = original(i, j, channel);
               }
            }
         }
      }

      // Move constructor (let's us put Images in std::vector)
      Image(Image &&other) : data(other.data), w(other.w), h(other.h), s(other.s), channels(other.channels), ownsData(other.ownsData)
      {
         other.data = nullptr;
      }

      int width() const { return w; }
      int height() const { return h; }
      int stride() const { return s; }
      int channelCount() const { return channels; }
      int planeSize() const { return h * s; }

      PixType &operator()(int i, int j, int channel = 0)
      {
         return data[channel * planeSize() + i * s + j];
      }

      const PixType &operator()(int i, int j, int channel = 0) const
      {
         return data[channel * planeSize() + i * s + j];
      }

      PixType *getData()
      {
         return data;
      }

      const PixType *getData() const
      {
         return data;
      }

      PixType *getChannelData(int channel)
      {
         return data + channel * planeSize();
      }

      const PixType *getChannelData(int channel) const
      {
         return data + channel * planeSize();
      }

      void write(const std::string &dest) const;

      ~Image()
      {
         if (ownsData && data)
            delete[] data;
      }

      // Forbid copy and assignment for now.
#ifdef WIN32
   private:
      // (Sadly "= delete" syntax does not work in MSVC2012)
      Image(const Image &);
      Image &operator=(const Image &);
#else
      Image(const Image &) = delete;
      Image &operator=(const Image &) = delete;
#endif
   };

}
