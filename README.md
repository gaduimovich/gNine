# G9 - Just In Time Image Processing With Eclipse OMR

An implementation of Luke Dodds Pixslam https://github.com/lukedodd/Pixslam using Eclipse OMR Jitbuilder.
See (README_ORIG.MD)

#### Carleton Univeristy Honours Project Winter 2019. Under the supervision of David Mould. ####

## Getting Started

### Installing

A step by step series of examples that tell you have to get a development env running
Requires cmake to be installed and a c++ compiler. Works on OS X and Ubuntu Linux

```
git clone --recurse-submodules https://github.com/gaduimovich/gNine
cd gNine
mkdir build ; cd build
cmake ..
make
```

### Running
Box Filter
```sh
./gnine ../examples/box_3x3.psm ../example_data/lena.png out.png
```
Danger Mode
```sh
./gnine ../examples/box_3x3.psm ../example_data/lena.png out.png --danger
```
Run Jited Function 20 times
```sh
./gnine ../examples/box_3x3.psm ../example_data/lena.png out.png --times=20
```

## Built With

* [Dropwizard](http://www.dropwizard.io/1.0.2/docs/) - The web framework used
* [Maven](https://maven.apache.org/) - Dependency Management
* [ROME](https://rometools.github.io/rome/) - Used to generate RSS Feeds


## Authors

* **Geoffrey Duimovich** - *Adding OMR* - (https://github.com/gduimovi)
* **Luke Dodd** - *Original Idea and base code* - (https://github.com/lukedodd)
## Acknowledgments
* Luke Dodd <https://github.com/lukedodd>
