#pragma once

#include <string>
#include <vector>

namespace pixslam{

class Image{
private:

    // TODO: think about dealing with different datatypes?
    // The image class is easy, but code generation on floats/doubles/chars and multichannels
    // could get complex!
    typedef double PixType;

    PixType *data;
    int w, h;
    int s;
    bool ownsData;

public:

    // Load from png image.
    // File errors result in a 0x0 sized image.
    Image(const std::string &path);

    // Construct new, zeroed, image.
    Image(int w, int h) : w(w), h(h), s(w), ownsData(true){
        data = new PixType[w*h];
        std::fill(data, data+(w*h), 0.0);
    }

    // Construct new, zeroed, image with specified stride.
    Image(int w_, int h_, int s_) : w(w_), h(h_), s(s_), ownsData(true){
        data = new PixType[h*s];
        std::fill(data, data+(h*s), 0.0);
    }


    // Construct image from already loaded data.
    // Does not take ownership, i.e. will not clean up data pointer.
    Image(PixType *data, int w, int h, int s)
        : data(data), w(w), h(h), s(s), ownsData(false)
    {
    }

    // Copy constructor
    Image(const Image &original, int padx, int pady)
        : w(original.width() + padx*2), h(original.height() + pady*2), s(w), ownsData(true){
        data = new PixType[w*h];
        std::fill(data, data+(w*h), 0.0);
        for(int i = 0; i < original.height(); ++i){
            for(int j = 0; j < original.width(); ++j){
                (*this)(i+pady, j+padx) = original(i,j);
            }
        }
    }

    // Forbid copy and assignment for now, allow move.
    Image(const Image &) = delete;
    Image &operator=(const Image&) = delete;

    // Move constructor - so we can put Images in std::vector
    Image(Image&& other) : 
        data(other.data), w(other.w), h(other.h), s(other.s), ownsData(other.ownsData){
        other.data = 0;
    }

    int width() const {return w;}
    int height() const {return h;}
    int stride() const {return s;}

    PixType &operator()(int i, int j){
        return data[i*s +j];
    }

    const PixType &operator()(int i, int j) const {
        return data[i*s +j];
    }

    PixType *getData(){
        return data;
    }

    const PixType *getData() const{
        return data;
    }

    void write(const std::string &dest) const;

    ~Image(){
        if(ownsData && data)
            delete [] data; 
    }

};

}
