/*******************************************************************************
 * This originates from Pixslam
 * Original Author is Luke Dodd
 ********************************************************************************/

#include "Image.h"

#include "stb_image.h"
#include "stb_image_write.h"

namespace gnine
{

   Image::Image(const std::string &path) : data(nullptr), w(0), h(0), s(0), channels(1), ownsData(true)
   {
      int n;
      unsigned char *datauc = stbi_load(path.c_str(), &w, &h, &n, 0);
      s = w;
      channels = (n >= 3) ? 3 : 1;

      if (w * h > 0)
      {
         data = new PixType[w * h * channels];
         if (channels == 1)
         {
            int sourceChannels = (n > 0) ? n : 1;
            for (int idx = 0; idx < w * h; ++idx)
               data[idx] = datauc[idx * sourceChannels] / 255.0;
         }
         else
         {
            for (int idx = 0; idx < w * h; ++idx)
            {
               for (int channel = 0; channel < channels; ++channel)
                  data[channel * w * h + idx] = datauc[idx * n + channel] / 255.0;
            }
         }
      }

      stbi_image_free(datauc);
   }

   void Image::write(const std::string &dest) const
   {
      std::vector<unsigned char> datauc(w * h * channels);

      for (int row = 0; row < h; ++row)
      {
         for (int col = 0; col < w; ++col)
         {
            int pixelIndex = row * w + col;
            if (channels == 1)
            {
               datauc[pixelIndex] = (unsigned char)(std::max(
                   std::min((*this)(row, col) * 255.0, 255.0), 0.0));
            }
            else
            {
               for (int channel = 0; channel < channels; ++channel)
               {
                  datauc[pixelIndex * channels + channel] = (unsigned char)(std::max(
                      std::min((*this)(row, col, channel) * 255.0, 255.0), 0.0));
               }
            }
         }
      }

      stbi_write_png(dest.c_str(), w, h, channels, &datauc[0], w * channels);
   }

}
