#include <cmath>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Parser.h"
#include "PreviewPlayback.hpp"
#include "Runtime.hpp"

namespace
{
   struct RuntimeCase
   {
      std::string name;
      std::string program;
      double expected;
   };

   double runProgram(const std::string &source)
   {
      gnine::runtime::Evaluator evaluator;
      gnine::runtime::Value result = evaluator.evaluateProgram(gnine::cellFromString(source));
      if (!result.isNumber())
         throw std::runtime_error("Program did not return a number");
      return result.number;
   }

   void require(bool condition, const std::string &message)
   {
      if (!condition)
         throw std::runtime_error(message);
   }

   bool almostEqual(double lhs, double rhs, double eps = 1e-9)
   {
      return std::fabs(lhs - rhs) <= eps;
   }

   bool contains(const std::string &text, const std::string &needle)
   {
      return text.find(needle) != std::string::npos;
   }

}

int main()
{
   try
   {
      const std::vector<RuntimeCase> cases = {
          {
              "closure_capture",
              "(() (define x 10) (define addx (lambda (y) (+ x y))) (addx 5))",
              15.0,
          },
          {
              "higher_order_lambda_argument",
              "(() ((lambda (f) (f 7)) (lambda (x) (+ x 3))))",
              10.0,
          },
          {
              "nested_closure_chain",
              "(() (define make-adder (lambda (x) (lambda (y) (+ x y)))) ((make-adder 4) 9))",
              13.0,
          },
          {
              "mod_positive",
              "(() (mod 7 3))",
              1.0,
          },
          {
              "mod_negative",
              "(() (mod -1 3))",
              2.0,
          },
          {
              "floor_positive",
              "(() (floor 3.7))",
              3.0,
          },
          {
              "floor_negative",
              "(() (floor -3.2))",
              -4.0,
          },
          {
              "ceil_positive",
              "(() (ceil 3.2))",
              4.0,
          },
          {
              "sqrt_basic",
              "(() (sqrt 9))",
              3.0,
          },
          {
              "cond_first_branch",
              "(() (cond (1 42) (0 99) (else 0)))",
              42.0,
          },
          {
              "cond_second_branch",
              "(() (cond (0 1) (1 99) (else 0)))",
              99.0,
          },
          {
              "cond_else",
              "(() (cond (0 1) (0 2) (else 7)))",
              7.0,
          },
          {
              "let_basic",
              "(() (let ((x 3) (y 4)) (+ x y)))",
              7.0,
          },
          {
              "let_star_sequential",
              "(() (let* ((x 3) (y (* x 2))) y))",
              6.0,
          },
          {
              "begin_last_value",
              "(() (begin 1 2 3))",
              3.0,
          },
          {
              "tuple_length",
              "(() (tuple-length (tuple 10 20 30)))",
              3.0,
          },
          {
              "tuple_set",
              "(() (get (tuple-set (tuple 1 2 3) 1 99) 1))",
              99.0,
          },
          {
              "tuple_slice",
              "(() (get (tuple-slice (tuple 10 20 30 40) 1 3) 0))",
              20.0,
          },
          {
              "array_length",
              "(() (array-length (array 10 20 30)))",
              3.0,
          },
          {
              "array_get",
              "(() (array-get (array 10 20 30) 1))",
              20.0,
          },
          {
              "array_set",
              "(() (array-get (array-set (array 1 2 3) 1 99) 1))",
              99.0,
          },
          {
              "array_push",
              "(() (array-get (array-push (array 1 2) 9) 2))",
              9.0,
          },
          {
              "array_pop",
              "(() (array-get (array-pop (array 10 20 30)) 1))",
              20.0,
          },
          {
              "array_slice",
              "(() (array-get (array-slice (array 10 20 30 40) 1 3) 0))",
              20.0,
          },
          {
              "array_fold",
              "(() (array-fold (lambda (acc x) (+ acc (* x 2))) 0 (array 1 2 3)))",
              12.0,
          },
          {
              "rand_of_deterministic",
              "(() (== (rand-of 42) (rand-of 42)))",
              1.0,
          },
          {
              "rand_of_different_seeds",
              "(() (not (== (rand-of 42) (rand-of 43))))",
              1.0,
          },
      };

      for (size_t idx = 0; idx < cases.size(); ++idx)
      {
         double result = runProgram(cases[idx].program);
         if (result != cases[idx].expected)
         {
            std::ostringstream out;
            out << "Runtime case " << cases[idx].name << " failed: expected "
                << cases[idx].expected << " but got " << result;
            throw std::runtime_error(out.str());
         }
      }

      {
         gnine::runtime::Heap heap;

         gnine::runtime::Value envRoot = gnine::runtime::Value::objectValue(heap.allocateEnvironment(NULL));
         gnine::runtime::Heap::Root root(heap, envRoot);
         gnine::runtime::EnvironmentObject *env =
             static_cast<gnine::runtime::EnvironmentObject *>(envRoot.object);

         gnine::Cell body = gnine::cellFromString("(+ x 1)");
         gnine::runtime::Value closureValue =
             gnine::runtime::Value::objectValue(heap.allocateClosure(std::vector<std::string>(),
                                                                     std::vector<gnine::Cell>(),
                                                                     body,
                                                                     env));
         gnine::runtime::Heap::Root closureRoot(heap, closureValue);

         env->bindings["keeper"] = closureValue;
         heap.collect();
         require(heap.liveObjectCount() == 2,
                 "collector reclaimed reachable environment/closure objects");
      }

      {
         gnine::runtime::Heap heap;

         gnine::runtime::Value envRoot = gnine::runtime::Value::objectValue(heap.allocateEnvironment(NULL));
         gnine::runtime::Heap::Root root(heap, envRoot);
         gnine::runtime::EnvironmentObject *env =
             static_cast<gnine::runtime::EnvironmentObject *>(envRoot.object);

         gnine::Cell body = gnine::cellFromString("(+ x 1)");
         heap.allocateClosure(std::vector<std::string>(),
                              std::vector<gnine::Cell>(),
                              body,
                              env);
         require(heap.liveObjectCount() == 2, "expected environment and closure before collection");

         heap.collect();
         require(heap.liveObjectCount() == 1,
                 "collector should reclaim unreachable closure while preserving rooted environment");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.0;
         imageA(0, 1) = 0.1;
         imageA(1, 0) = 0.2;
         imageA(1, 1) = 0.3;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["bias"] = gnine::runtime::Value::numberValue(0.25);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (+ x bias)) A)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "map-image over runtime image should return an image object");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->width() == 2 && imageObj->image->height() == 2,
                 "runtime image object preserved image extent");
         require(imageObj->image->channelCount() == 1,
                 "runtime map-image preserved grayscale channel count");
         require(almostEqual(imageObj->image->operator()(0, 0), 0.25) &&
                 almostEqual(imageObj->image->operator()(0, 1), 0.35) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.45) &&
                 almostEqual(imageObj->image->operator()(1, 1), 0.55),
                 "runtime map-image produced expected pixels");
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=compiled") &&
                 contains(trace[0], "cache=") &&
                 contains(trace[0], "compile_ms=") &&
                 contains(trace[0], "execute_ms="),
                 "runtime map-image should trace compiled execution");

         gnine::runtime::Value widthValue = evaluator.evaluateExpr(gnine::cellFromString("(width A)"), bindings);
         gnine::runtime::Value heightValue = evaluator.evaluateExpr(gnine::cellFromString("(height A)"), bindings);
         gnine::runtime::Value channelValue = evaluator.evaluateExpr(gnine::cellFromString("(channels A)"), bindings);
         require(widthValue.isNumber() && almostEqual(widthValue.number, 2.0),
                 "width builtin should work on runtime image objects");
         require(heightValue.isNumber() && almostEqual(heightValue.number, 2.0),
                 "height builtin should work on runtime image objects");
         require(channelValue.isNumber() && almostEqual(channelValue.number, 1.0),
                 "channels builtin should work on runtime image objects");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         gnine::Image imageB(2, 2);
         imageA(0, 0) = 0.0;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.4;
         imageA(1, 1) = 0.6;
         imageB(0, 0) = 1.0;
         imageB(0, 1) = 0.8;
         imageB(1, 0) = 0.6;
         imageB(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["B"] = evaluator.imageValue(imageB);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("((lambda (w) (zip-image (lambda (x y) (+ (* w x) (* (- 1 w) y))) A B)) 0.25)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "zip-image with captured image bindings should return an image object");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(almostEqual(imageObj->image->operator()(0, 0), 0.75) &&
                 almostEqual(imageObj->image->operator()(0, 1), 0.65) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.55) &&
                 almostEqual(imageObj->image->operator()(1, 1), 0.45),
                 "runtime zip-image closure produced expected pixels");
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.zip_image.mode=compiled") &&
                 contains(trace[0], "cache=") &&
                 contains(trace[0], "compile_ms=") &&
                 contains(trace[0], "execute_ms="),
                 "runtime zip-image should trace compiled execution");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image trail(2, 2);
         trail(0, 0) = 0.1;
         trail(0, 1) = 0.2;
         trail(1, 0) = 0.3;
         trail(1, 1) = 0.4;

         gnine::Image frame(2, 2, 2, 3);
         frame(0, 0, 0) = 1.0;
         frame(0, 1, 0) = 2.0;
         frame(1, 0, 0) = 3.0;
         frame(1, 1, 0) = 4.0;
         frame(0, 0, 1) = 10.0;
         frame(0, 1, 1) = 20.0;
         frame(1, 0, 1) = 30.0;
         frame(1, 1, 1) = 40.0;
         frame(0, 0, 2) = 100.0;
         frame(0, 1, 2) = 200.0;
         frame(1, 0, 2) = 300.0;
         frame(1, 1, 2) = 400.0;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["trail"] = evaluator.imageValue(trail);
         bindings["frame"] = evaluator.imageValue(frame);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(zip-image (lambda (age color) (+ color (* 0.5 age))) trail frame)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "zip-image should support grayscale-to-rgb broadcast");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->channelCount() == 3,
                 "zip-image broadcast result should preserve rgb output");
         require(almostEqual(imageObj->image->operator()(0, 0, 0), 1.05) &&
                 almostEqual(imageObj->image->operator()(0, 1, 0), 2.10) &&
                 almostEqual(imageObj->image->operator()(1, 0, 0), 3.15) &&
                 almostEqual(imageObj->image->operator()(1, 1, 0), 4.20) &&
                 almostEqual(imageObj->image->operator()(0, 0, 1), 10.05) &&
                 almostEqual(imageObj->image->operator()(0, 1, 1), 20.10) &&
                 almostEqual(imageObj->image->operator()(1, 0, 1), 30.15) &&
                 almostEqual(imageObj->image->operator()(1, 1, 1), 40.20) &&
                 almostEqual(imageObj->image->operator()(0, 0, 2), 100.05) &&
                 almostEqual(imageObj->image->operator()(0, 1, 2), 200.10) &&
                 almostEqual(imageObj->image->operator()(1, 0, 2), 300.15) &&
                 almostEqual(imageObj->image->operator()(1, 1, 2), 400.20),
                 "zip-image broadcast should reuse grayscale values across rgb channels");
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.zip_image.mode=compiled"),
                 "zip-image broadcast should stay on the compiled path");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.0;
         imageA(0, 1) = 1.0;
         imageA(1, 0) = 2.0;
         imageA(1, 1) = 3.0;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (+ (* 10 i) j x)) A)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "runtime map-image should expose i and j inside closures");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(almostEqual(imageObj->image->operator()(0, 0), 0.0) &&
                 almostEqual(imageObj->image->operator()(0, 1), 2.0) &&
                 almostEqual(imageObj->image->operator()(1, 0), 12.0) &&
                 almostEqual(imageObj->image->operator()(1, 1), 14.0),
                 "runtime map-image coordinate bindings produced expected pixels");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 1.0;
         imageA(0, 1) = 2.0;
         imageA(1, 0) = 3.0;
         imageA(1, 1) = 4.0;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (- (A -1 0) x)) A)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "runtime map-image should allow callable image sampling");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(almostEqual(imageObj->image->operator()(0, 0), 2.0) &&
                 almostEqual(imageObj->image->operator()(0, 1), 2.0) &&
                 almostEqual(imageObj->image->operator()(1, 0), -2.0) &&
                 almostEqual(imageObj->image->operator()(1, 1), -2.0),
                 "runtime image sampling should use reflected relative coordinates");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(3, 2, 3, 3);
         imageA(0, 0, 0) = 0.1;
         imageA(0, 0, 1) = 0.2;
         imageA(0, 0, 2) = 0.3;
         imageA(0, 1, 0) = 1.1;
         imageA(0, 1, 1) = 1.2;
         imageA(0, 1, 2) = 1.3;
         imageA(0, 2, 0) = 2.1;
         imageA(0, 2, 1) = 2.2;
         imageA(0, 2, 2) = 2.3;
         imageA(1, 0, 0) = 10.1;
         imageA(1, 0, 1) = 10.2;
         imageA(1, 0, 2) = 10.3;
         imageA(1, 1, 0) = 11.1;
         imageA(1, 1, 1) = 11.2;
         imageA(1, 1, 2) = 11.3;
         imageA(1, 2, 0) = 12.1;
         imageA(1, 2, 1) = 12.2;
         imageA(1, 2, 2) = 12.3;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value defaultChannel =
             evaluator.evaluateExpr(gnine::cellFromString("(sample-image A 2 1)"), bindings);
         gnine::runtime::Value explicitChannel =
             evaluator.evaluateExpr(gnine::cellFromString("(sample-image A 2 1 2)"), bindings);
         gnine::runtime::Value reflectedNegativeCoord =
             evaluator.evaluateExpr(gnine::cellFromString("(sample-image A -1 2 1)"), bindings);
         gnine::runtime::Value reflectedPositiveCoordAndChannel =
             evaluator.evaluateExpr(gnine::cellFromString("(sample-image A 4 3 5)"), bindings);

         require(defaultChannel.isNumber() && almostEqual(defaultChannel.number, 12.1),
                 "sample-image should read absolute coordinates from channel 0 by default");
         require(explicitChannel.isNumber() && almostEqual(explicitChannel.number, 12.3),
                 "sample-image should support explicit channel reads");
         require(reflectedNegativeCoord.isNumber() && almostEqual(reflectedNegativeCoord.number, 11.2),
                 "sample-image should reflect negative absolute coordinates with the shared border policy");
         require(reflectedPositiveCoordAndChannel.isNumber() &&
                     almostEqual(reflectedPositiveCoordAndChannel.number, 1.1),
                 "sample-image should reflect oversized coordinates and channel indices");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image grayscale(2, 2);
         grayscale(0, 0) = 0.25;
         grayscale(0, 1) = 0.50;
         grayscale(1, 0) = 0.75;
         grayscale(1, 1) = 1.00;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(grayscale);

         gnine::runtime::Value forcedChannel =
             evaluator.evaluateExpr(gnine::cellFromString("(sample-image A 1 1 7)"), bindings);

         require(forcedChannel.isNumber() && almostEqual(forcedChannel.number, 1.00),
                 "sample-image should ignore explicit channel indices for grayscale images");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.25;
         imageA(0, 1) = 0.50;
         imageA(1, 0) = 0.75;
         imageA(1, 1) = 1.00;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("((A) (define threshold 0.5) (map-image (lambda (x) (if (> x threshold) 1 0)) A))"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "evaluateProgram with bound image arguments should return an image object");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(almostEqual(imageObj->image->operator()(0, 0), 0.0) &&
                 almostEqual(imageObj->image->operator()(0, 1), 0.0) &&
                 almostEqual(imageObj->image->operator()(1, 0), 1.0) &&
                 almostEqual(imageObj->image->operator()(1, 1), 1.0),
                 "runtime program should support comparison builtins inside image lambdas");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(3, 2, 3, 3);

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["iter"] = gnine::runtime::Value::numberValue(4.0);

         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("((A) (+ (width A) (height A) (channels A) iter))"),
             bindings);
         require(result.isNumber() && almostEqual(result.number, 12.0),
                 "runtime program bindings should support scalar metadata queries and iter");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(map-image + A)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "runtime map-image with builtin callable should still return an image object");

         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=interpreted") &&
                 contains(trace[0], "fallback=non_closure") &&
                 contains(trace[0], "execute_ms="),
                 "runtime map-image should trace interpreted fallback mode");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["__cache_probe_bias__"] = gnine::runtime::Value::numberValue(0.125);

         evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (+ x __cache_probe_bias__)) A)"),
             bindings);
         evaluator.clearExecutionTrace();

         evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (+ x __cache_probe_bias__)) A)"),
             bindings);
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=compiled") &&
                 contains(trace[0], "cache=hit"),
                 "runtime map-image should reuse cached compiled kernels");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["iter"] = gnine::runtime::Value::numberValue(1.0);

         evaluator.evaluateProgram(
             gnine::cellFromString("((A) (define xoff (+ iter 1)) (map-image (lambda (x) (+ x xoff)) A))"),
             bindings);
         evaluator.clearExecutionTrace();

         bindings["iter"] = gnine::runtime::Value::numberValue(2.0);
         evaluator.evaluateProgram(
             gnine::cellFromString("((A) (define xoff (+ iter 1)) (map-image (lambda (x) (+ x xoff)) A))"),
             bindings);
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=compiled") &&
                 contains(trace[0], "cache=hit"),
                 "runtime compiled map-image should keep iter-driven scalar captures dynamic");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString(
                 "(canvas 5 2 "
                 "  (if (< j 2) "
                 "    (A 0 0) "
                 "    (if (== j 2) 1 (A 0 -3))))"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "canvas should return an image object");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->width() == 5 && imageObj->image->height() == 2,
                 "canvas should honor explicit output dimensions");
         require(imageObj->image->channelCount() == 1,
                 "canvas should default to a single channel");
         require(almostEqual(imageObj->image->operator()(0, 0), 0.1) &&
                 almostEqual(imageObj->image->operator()(0, 1), 0.2) &&
                 almostEqual(imageObj->image->operator()(0, 2), 1.0) &&
                 almostEqual(imageObj->image->operator()(0, 3), 0.1) &&
                 almostEqual(imageObj->image->operator()(0, 4), 0.2) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.3) &&
                 almostEqual(imageObj->image->operator()(1, 1), 0.4) &&
                 almostEqual(imageObj->image->operator()(1, 2), 1.0) &&
                 almostEqual(imageObj->image->operator()(1, 3), 0.3) &&
                 almostEqual(imageObj->image->operator()(1, 4), 0.4),
                 "canvas should support larger side-by-side compositions");

         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.canvas.mode=compiled") &&
                 contains(trace[0], "width=5") &&
                 contains(trace[0], "height=2") &&
                 contains(trace[0], "channels=1"),
                 "canvas should trace compiled rendering");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(1, 1, 1, 3);
         imageA(0, 0, 0) = 0.2;
         imageA(0, 0, 1) = 0.4;
         imageA(0, 0, 2) = 0.6;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(canvas 1 1 3 (+ (A 0 0) (* 0.1 c)))"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "canvas with explicit channels should return an image object");

         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->channelCount() == 3,
                 "canvas should honor explicit channel count");
         require(almostEqual(imageObj->image->operator()(0, 0, 0), 0.2) &&
                 almostEqual(imageObj->image->operator()(0, 0, 1), 0.5) &&
                 almostEqual(imageObj->image->operator()(0, 0, 2), 0.8),
                 "canvas should expose c and sample multi-channel images");
         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.canvas.mode=compiled") &&
                 contains(trace[0], "fused=rgb"),
                 "sampled RGB canvas should use the fused compiled path");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);
         bindings["iter"] = gnine::runtime::Value::numberValue(1.0);

         gnine::runtime::Value state = evaluator.evaluateProgram(
             gnine::cellFromString(
                 "((A) "
                 "  (define x 3) "
                 "  (define y 2) "
                 "  (define next-frame "
                 "    (canvas 8 6 3 "
                 "      (max "
                 "        (if (== c 0) 0.03 (if (== c 1) 0.05 0.08)) "
                 "        (* (draw-rect x y 1 1 1.0) "
                 "           (if (== c 0) 0.16 (if (== c 1) 0.92 1.00)))))) "
                 "  (tuple next-frame (+ x 1) y))"),
             bindings);

         require(state.isObject() && state.object->type == gnine::runtime::Object::Tuple,
                 "runtime program should support define-bound canvas values returned via tuple state");
         gnine::runtime::TupleObject *tupleObj = static_cast<gnine::runtime::TupleObject *>(state.object);
         require(tupleObj->values.size() == 3,
                 "define-bound canvas tuple state should keep all tuple fields");
         require(tupleObj->values[0].isObject() &&
                 tupleObj->values[0].object->type == gnine::runtime::Object::Image,
                 "define-bound canvas tuple state should keep the rendered image as the first field");
         require(tupleObj->values[1].isNumber() && almostEqual(tupleObj->values[1].number, 4.0) &&
                 tupleObj->values[2].isNumber() && almostEqual(tupleObj->values[2].number, 2.0),
                 "define-bound canvas tuple state should preserve trailing scalar fields");

         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.canvas.mode=compiled") &&
                 contains(trace[0], "fused=rgb"),
                 "define-bound canvas tuple state should still use the compiled canvas path");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("(() (define state (tuple 7 9)) (+ (get state 0) (get state 1)))"));
         require(result.isNumber() && almostEqual(result.number, 16.0),
                 "tuple/get builtins should support non-image runtime state");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("(() (array 7 9 11))"));
         require(result.isObject() && result.object->type == gnine::runtime::Object::Array,
                 "array builtin should produce a dedicated runtime array object");
         gnine::runtime::ArrayObject *arrayObj =
             static_cast<gnine::runtime::ArrayObject *>(result.object);
         require(arrayObj->values.size() == 3,
                 "runtime array object should retain all elements");
         require(arrayObj->values[2].isNumber() && almostEqual(arrayObj->values[2].number, 11.0),
                 "runtime array object should preserve appended scalar values");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("((pair) ((lambda ((x y)) (+ x y)) pair))"),
             std::map<std::string, gnine::runtime::Value>{
                 {"pair", gnine::runtime::Value::objectValue(
                              evaluator.heap().allocateTuple(std::vector<gnine::runtime::Value>{
                                  gnine::runtime::Value::numberValue(6.0),
                                  gnine::runtime::Value::numberValue(7.0)}))}});
         require(result.isNumber() && almostEqual(result.number, 13.0),
                 "lambda parameters should support tuple destructuring");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("((((x y) z)) (+ x y z))"),
             std::map<std::string, gnine::runtime::Value>{
                 {"__arg0__", gnine::runtime::Value::objectValue(
                                  evaluator.heap().allocateTuple(std::vector<gnine::runtime::Value>{
                                      gnine::runtime::Value::objectValue(
                                          evaluator.heap().allocateTuple(std::vector<gnine::runtime::Value>{
                                              gnine::runtime::Value::numberValue(2.0),
                                              gnine::runtime::Value::numberValue(3.0)})),
                                      gnine::runtime::Value::numberValue(4.0)}))}});
         require(result.isNumber() && almostEqual(result.number, 9.0),
                 "top-level runtime arguments should support nested tuple destructuring");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::vector<gnine::runtime::Value> stateValues;
         stateValues.push_back(evaluator.imageValue(imageA));
         stateValues.push_back(gnine::runtime::Value::numberValue(0.25));
         gnine::runtime::Value state =
             gnine::runtime::Value::objectValue(evaluator.heap().allocateTuple(stateValues));

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["state"] = state;

         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString(
                 "((state) "
                 "(define frame (get state 0)) "
                 "(define bias (get state 1)) "
                 "(define state 0) "
                 "(map-image (lambda (x) (+ x bias)) frame))"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "tuple-derived scalar captures should still produce an image");

         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=compiled"),
                 "tuple-derived scalar captures should compile in runtime map-image");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.1;
         imageA(0, 1) = 0.2;
         imageA(1, 0) = 0.3;
         imageA(1, 1) = 0.4;

         std::vector<gnine::runtime::Value> stateValuesA;
         stateValuesA.push_back(evaluator.imageValue(imageA));
         stateValuesA.push_back(gnine::runtime::Value::numberValue(0.25));
         gnine::runtime::Value stateA =
             gnine::runtime::Value::objectValue(evaluator.heap().allocateTuple(stateValuesA));

         std::vector<gnine::runtime::Value> stateValuesB;
         stateValuesB.push_back(evaluator.imageValue(imageA));
         stateValuesB.push_back(gnine::runtime::Value::numberValue(0.5));
         gnine::runtime::Value stateB =
             gnine::runtime::Value::objectValue(evaluator.heap().allocateTuple(stateValuesB));

         evaluator.evaluateProgram(
             gnine::cellFromString(
                 "((state) "
                 "(define frame (get state 0)) "
                 "(define bias (get state 1)) "
                 "(define state 0) "
                 "(map-image (lambda (x) (+ x bias)) frame))"),
             std::map<std::string, gnine::runtime::Value>{{"state", stateA}});
         evaluator.clearExecutionTrace();

         evaluator.evaluateProgram(
             gnine::cellFromString(
                 "((state) "
                 "(define frame (get state 0)) "
                 "(define bias (get state 1)) "
                 "(define state 0) "
                 "(map-image (lambda (x) (+ x bias)) frame))"),
             std::map<std::string, gnine::runtime::Value>{{"state", stateB}});

         const std::vector<std::string> &trace = evaluator.executionTrace();
         require(!trace.empty() &&
                 contains(trace[0], "runtime.map_image.mode=compiled") &&
                 contains(trace[0], "cache=hit"),
                 "stateful tuple-derived scalar captures should reuse the compiled runtime kernel");
      }

      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 2);
         imageA(0, 0) = 0.0;
         imageA(0, 1) = 0.1;
         imageA(1, 0) = 0.2;
         imageA(1, 1) = 0.3;

         std::vector<gnine::runtime::Value> initialStateValues;
         initialStateValues.push_back(evaluator.imageValue(imageA));
         initialStateValues.push_back(gnine::runtime::Value::numberValue(0.0));
         gnine::runtime::Value state =
             gnine::runtime::Value::objectValue(evaluator.heap().allocateTuple(initialStateValues));
         gnine::runtime::Heap::Root stateRoot(evaluator.heap(), state);

         for (int iter = 1; iter <= 2; ++iter)
         {
            std::map<std::string, gnine::runtime::Value> bindings;
            bindings["state"] = state;
            bindings["iter"] = gnine::runtime::Value::numberValue(iter);
            state = evaluator.evaluateProgram(
                gnine::cellFromString(
                    "((state) "
                    "(define frame (get state 0)) "
                    "(define bias (get state 1)) "
                    "(tuple (map-image (lambda (x) (+ x bias)) frame) (+ bias 0.25)))"),
                bindings);
         }

         require(state.isObject() && state.object->type == gnine::runtime::Object::Tuple,
                 "runtime state iteration should preserve tuple outputs");
         gnine::runtime::TupleObject *tupleObj = static_cast<gnine::runtime::TupleObject *>(state.object);
         require(tupleObj->values.size() == 2,
                 "runtime tuple state should retain both image and scalar fields");
         require(tupleObj->values[1].isNumber() && almostEqual(tupleObj->values[1].number, 0.5),
                 "runtime tuple state should carry scalar fields across iterations");
         require(tupleObj->values[0].isObject() && tupleObj->values[0].object->type == gnine::runtime::Object::Image,
                 "runtime tuple state should carry image fields across iterations");

         gnine::runtime::ImageObject *imageObj =
             static_cast<gnine::runtime::ImageObject *>(tupleObj->values[0].object);
         require(almostEqual(imageObj->image->operator()(0, 0), 0.25) &&
                 almostEqual(imageObj->image->operator()(0, 1), 0.35) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.45) &&
                 almostEqual(imageObj->image->operator()(1, 1), 0.55),
                 "runtime tuple state should evolve the image field across chained evaluations");
      }

      {
         gnine::PreviewPlaybackScenario scenario = gnine::PreviewPlaybackScenario::None;
         require(gnine::parsePreviewPlaybackScenario("snake", &scenario) &&
                     scenario == gnine::PreviewPlaybackScenario::Snake,
                 "snake playback scenario should parse");
         require(gnine::parsePreviewPlaybackScenario("pong", &scenario) &&
                     scenario == gnine::PreviewPlaybackScenario::Pong,
                 "pong playback scenario should parse");
         require(!gnine::parsePreviewPlaybackScenario("nope", &scenario),
                 "invalid playback scenarios should be rejected");
         require(gnine::previewPlaybackFrameBudget(10000.0, 1000.0 / 60.0) == 600,
                 "10 second playback should map to 600 frames at 60fps");

         gnine::RuntimeInputState snakeFrame0 = gnine::makePreviewPlaybackInput(gnine::PreviewPlaybackScenario::Snake, 0);
         gnine::RuntimeInputState snakeFrame33 = gnine::makePreviewPlaybackInput(gnine::PreviewPlaybackScenario::Snake, 33);
         gnine::RuntimeInputState snakeFrame60 = gnine::makePreviewPlaybackInput(gnine::PreviewPlaybackScenario::Snake, 60);
         gnine::RuntimeInputState pongFrame0 = gnine::makePreviewPlaybackInput(gnine::PreviewPlaybackScenario::Pong, 0);
         gnine::RuntimeInputState pongFrame46 = gnine::makePreviewPlaybackInput(gnine::PreviewPlaybackScenario::Pong, 46);

         require(snakeFrame0.keyRight == 1.0 && snakeFrame0.keyD == 1.0,
                 "snake playback should start by steering right");
         require(snakeFrame33.keyUp == 1.0 && snakeFrame33.keyW == 1.0,
                 "snake playback should turn upward in the scripted path");
         require(snakeFrame60.keyLeft == 1.0 && snakeFrame60.keyA == 1.0,
                 "snake playback should turn left in the scripted path");
         require(pongFrame0.keyUp == 1.0, "pong playback should start with an upward paddle move");
         require(pongFrame46.keyDown == 1.0, "pong playback should alternate to downward paddle motion");
      }

      // Math builtins: sin, cos, pow, atan2
      {
         require(almostEqual(runProgram("(() (sin 0.0))"), 0.0),
                 "sin(0) should return 0");
         require(almostEqual(runProgram("(() (cos 0.0))"), 1.0),
                 "cos(0) should return 1");
         require(almostEqual(runProgram("(() (pow 2.0 3.0))"), 8.0),
                 "pow(2,3) should return 8");
         require(almostEqual(runProgram("(() (atan2 0.0 1.0))"), 0.0),
                 "atan2(0,1) should return 0");
      }

      // Numeric builtins: abs, clamp, int
      {
         require(almostEqual(runProgram("(() (abs 3.5))"), 3.5),
                 "abs of positive should return the same value");
         require(almostEqual(runProgram("(() (abs -3.5))"), 3.5),
                 "abs of negative should return the positive value");
         require(almostEqual(runProgram("(() (clamp -0.5 0.0 1.0))"), 0.0),
                 "clamp below range should return the lower bound");
         require(almostEqual(runProgram("(() (clamp 1.5 0.0 1.0))"), 1.0),
                 "clamp above range should return the upper bound");
         require(almostEqual(runProgram("(() (clamp 0.3 0.0 1.0))"), 0.3),
                 "clamp inside range should return the value unchanged");
         require(almostEqual(runProgram("(() (int 3.9))"), 3.0),
                 "int truncation of positive should round toward zero");
         require(almostEqual(runProgram("(() (int -3.9))"), -3.0),
                 "int truncation of negative should round toward zero");
      }

      // Boolean builtins: not, and, or
      {
         require(almostEqual(runProgram("(() (not 1.0))"), 0.0),
                 "not of truthy value should return 0");
         require(almostEqual(runProgram("(() (not 0.0))"), 1.0),
                 "not of falsy value should return 1");
         require(almostEqual(runProgram("(() (and 1 1))"), 1.0),
                 "and of two truthy values should return 1");
         require(almostEqual(runProgram("(() (and 1 0))"), 0.0),
                 "and with one false should return 0");
         require(almostEqual(runProgram("(() (or 0 1))"), 1.0),
                 "or with one true should return 1");
         require(almostEqual(runProgram("(() (or 0 0))"), 0.0),
                 "or of two false values should return 0");
      }

      // min and max with multiple arguments
      {
         require(almostEqual(runProgram("(() (min 3 1 2))"), 1.0),
                 "min of multiple values should return the smallest");
         require(almostEqual(runProgram("(() (max 3 1 2))"), 3.0),
                 "max of multiple values should return the largest");
      }

      // draw-circle in canvas context
      {
         gnine::runtime::Evaluator evaluator;
         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(canvas 3 2 (draw-circle 1.0 0.0 1.0 1.0))"),
             std::map<std::string, gnine::runtime::Value>{});
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "draw-circle inside canvas should return an image");
         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->width() == 3 && imageObj->image->height() == 2,
                 "draw-circle canvas result should have the requested dimensions");
         // circle centered at col=1, row=0, radius=1:
         // row0: col0→dist2=1→inside, col1→dist2=0→inside, col2→dist2=1→inside
         // row1: col0→dist2=2→outside, col1→dist2=1→inside, col2→dist2=2→outside
         require(almostEqual(imageObj->image->operator()(0, 0), 1.0) &&
                 almostEqual(imageObj->image->operator()(0, 1), 1.0) &&
                 almostEqual(imageObj->image->operator()(0, 2), 1.0) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.0) &&
                 almostEqual(imageObj->image->operator()(1, 1), 1.0) &&
                 almostEqual(imageObj->image->operator()(1, 2), 0.0),
                 "draw-circle should paint pixels within the specified radius");
      }

      // draw-line in canvas context
      {
         gnine::runtime::Evaluator evaluator;
         // vertical line at column=1, from row=0 to row=1, thickness=0.5 (halfThick=0.25)
         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(canvas 3 2 (draw-line 1.0 0.0 1.0 1.0 0.5 1.0))"),
             std::map<std::string, gnine::runtime::Value>{});
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "draw-line inside canvas should return an image");
         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         // only pixels at column=1 are within distance 0.25 of the vertical line
         require(almostEqual(imageObj->image->operator()(0, 0), 0.0) &&
                 almostEqual(imageObj->image->operator()(0, 1), 1.0) &&
                 almostEqual(imageObj->image->operator()(0, 2), 0.0) &&
                 almostEqual(imageObj->image->operator()(1, 0), 0.0) &&
                 almostEqual(imageObj->image->operator()(1, 1), 1.0) &&
                 almostEqual(imageObj->image->operator()(1, 2), 0.0),
                 "draw-line should paint pixels within the specified thickness");
      }

      // normalizeProgram
      {
         gnine::runtime::Evaluator evaluator;
         gnine::Cell program = gnine::cellFromString("((A) (define x (+ A 1)) (* x x))");
         gnine::Cell normalized = evaluator.normalizeProgram(program);
         require(normalized.type == gnine::Cell::List && normalized.list.size() == 3,
                 "normalizeProgram should produce a three-element program list");
         require(normalized.list[0].type == gnine::Cell::List,
                 "normalizeProgram should preserve the argument list");
         require(normalized.list[1].type == gnine::Cell::List &&
                 normalized.list[1].list.size() == 3 &&
                 normalized.list[1].list[0].type == gnine::Cell::Symbol &&
                 normalized.list[1].list[0].val == "define",
                 "normalizeProgram should preserve define statements");
         require(normalized.list[2].type == gnine::Cell::List,
                 "normalizeProgram should produce a result expression");
      }

      // evaluateProgram with outBindings
      {
         gnine::runtime::Evaluator evaluator;
         std::map<std::string, gnine::runtime::Value> outBindings;
         gnine::runtime::Value result = evaluator.evaluateProgram(
             gnine::cellFromString("(() (define x 5) (define y 7) (+ x y))"),
             std::map<std::string, gnine::runtime::Value>{},
             &outBindings);
         require(result.isNumber() && almostEqual(result.number, 12.0),
                 "evaluateProgram with outBindings should return the correct result");
         require(outBindings.count("x") > 0 && outBindings.at("x").isNumber() &&
                     almostEqual(outBindings.at("x").number, 5.0),
                 "evaluateProgram outBindings should contain the define for x");
         require(outBindings.count("y") > 0 && outBindings.at("y").isNumber() &&
                     almostEqual(outBindings.at("y").number, 7.0),
                 "evaluateProgram outBindings should contain the define for y");
      }

      // GC collection count tracking
      {
         gnine::runtime::Heap heap;
         require(heap.collectionCount() == 0,
                 "fresh heap should report zero collections");
         heap.collect();
         require(heap.collectionCount() == 1,
                 "heap collection count should increment after explicit collect");
         heap.collect();
         require(heap.collectionCount() == 2,
                 "heap collection count should increment on each collect call");
      }

      // rgb builtin in map-image (produces a 3-channel image)
      {
         gnine::runtime::Evaluator evaluator;
         gnine::Image imageA(2, 1, 2, 3);
         imageA(0, 0, 0) = 0.1;
         imageA(0, 0, 1) = 0.5;
         imageA(0, 0, 2) = 0.9;
         imageA(0, 1, 0) = 0.2;
         imageA(0, 1, 1) = 0.6;
         imageA(0, 1, 2) = 0.8;

         std::map<std::string, gnine::runtime::Value> bindings;
         bindings["A"] = evaluator.imageValue(imageA);

         gnine::runtime::Value result = evaluator.evaluateExpr(
             gnine::cellFromString("(map-image (lambda (x) (rgb (* x 2) x 0.0)) A)"),
             bindings);
         require(result.isObject() && result.object->type == gnine::runtime::Object::Image,
                 "map-image returning rgb should produce an image");
         gnine::runtime::ImageObject *imageObj = static_cast<gnine::runtime::ImageObject *>(result.object);
         require(imageObj->image->channelCount() == 3,
                 "map-image returning rgb should produce a 3-channel image");
      }

      std::cout << "Runtime tests passed" << std::endl;
      return 0;
   }
   catch (const std::exception &ex)
   {
      std::cerr << ex.what() << std::endl;
      return 1;
   }
}
