#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Parser.h"
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
             gnine::runtime::Value::objectValue(heap.allocateClosure(std::vector<std::string>(), body, env));
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
         heap.allocateClosure(std::vector<std::string>(), body, env);
         require(heap.liveObjectCount() == 2, "expected environment and closure before collection");

         heap.collect();
         require(heap.liveObjectCount() == 1,
                 "collector should reclaim unreachable closure while preserving rooted environment");
      }

      std::cout << "Runtime tests passed (" << cases.size() + 2 << " checks)" << std::endl;
      return 0;
   }
   catch (const std::exception &ex)
   {
      std::cerr << ex.what() << std::endl;
      return 1;
   }
}
