#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ImageArray.hpp"
#include "JitBuilder.hpp"
#include "Parser.h"
#include "VectorProgram.hpp"

namespace
{
   struct ParserCase
   {
      std::string name;
      std::string source;
      std::string expectedPrinted;
   };

   struct ProgramCase
   {
      std::string name;
      std::string code;
      int width;
      int height;
      std::vector<std::vector<double>> inputs;
      std::vector<double> expected;
      int chainTimes;
      bool danger;
   };

   bool almostEqual(double lhs, double rhs, double eps = 1e-9)
   {
      return std::fabs(lhs - rhs) <= eps;
   }

   bool isTopLevelIterate(const gnine::Cell &cell)
   {
      return cell.type == gnine::Cell::List &&
             cell.list.size() == 3 &&
             cell.list[0].type == gnine::Cell::Symbol &&
             cell.list[0].val == "iterate" &&
             cell.list[1].type == gnine::Cell::Number &&
             cell.list[2].type == gnine::Cell::List;
   }

   int parseIterateCount(const gnine::Cell &cell)
   {
      std::stringstream ss(cell.list[1].val);
      int value = 0;
      ss >> value;
      char trailing = '\0';
      if (!ss || (ss >> trailing) || value <= 0)
         throw std::runtime_error("iterate count must be a positive integer");
      return value;
   }

   std::string formatVector(const std::vector<double> &values, int width)
   {
      std::ostringstream out;
      out << std::fixed << std::setprecision(6);
      for (size_t idx = 0; idx < values.size(); ++idx)
      {
         if (idx > 0)
         {
            if (width > 0 && idx % static_cast<size_t>(width) == 0)
               out << " | ";
            else
               out << ", ";
         }
         out << values[idx];
      }
      return out.str();
   }

   std::vector<double> runProgram(const ProgramCase &program)
   {
      gnine::Cell code = gnine::cellFromString(program.code);
      gnine::Cell effectiveCode = code;
      int iterateCount = 0;
      if (isTopLevelIterate(code))
      {
         iterateCount = parseIterateCount(code);
         effectiveCode = code.list[2];
      }

      gnine::LoweredProgram lowered = gnine::lowerProgram(effectiveCode);
      if (lowered.usesVectorFeatures || lowered.channelPrograms.size() != 1)
         throw std::runtime_error("Semantic tests expect a single scalar lowered program");

      OMR::JitBuilder::TypeDictionary types;
      ImageArray method(&types);
      method.runByteCodes(lowered.channelPrograms[0], program.danger);

      void *entry = nullptr;
      int32_t rc = compileMethodBuilder(&method, &entry);
      if (rc != 0)
         throw std::runtime_error("compileMethodBuilder failed with rc=" + std::to_string(rc));

      auto *fn = reinterpret_cast<ImageArrayFunctionType *>(entry);
      std::vector<std::vector<double>> mutableInputs = program.inputs;
      std::vector<double *> dataPtrs;
      std::vector<int32_t> inputWidths;
      std::vector<int32_t> inputHeights;
      std::vector<int32_t> inputStrides;
      for (std::vector<double> &input : mutableInputs)
      {
         if (input.size() != static_cast<size_t>(program.width * program.height))
            throw std::runtime_error("Input size mismatch in case " + program.name);
         dataPtrs.push_back(input.data());
         inputWidths.push_back(program.width);
         inputHeights.push_back(program.height);
         inputStrides.push_back(program.width);
      }

      std::vector<double> output(program.width * program.height, 0.0);

      int effectiveChainTimes = program.chainTimes > 0 ? program.chainTimes : iterateCount;

      if (effectiveChainTimes > 0)
      {
         if (mutableInputs.size() != 1)
            throw std::runtime_error("Chained tests require exactly one input image");

         std::vector<double> chainInput = mutableInputs[0];
         std::vector<double> chainOutput(program.width * program.height, 0.0);
         std::vector<double *> chainPtrs(1, chainInput.data());
         std::vector<int32_t> chainWidths(1, program.width);
         std::vector<int32_t> chainHeights(1, program.height);
         std::vector<int32_t> chainStrides(1, program.width);

         for (int iter = 0; iter < effectiveChainTimes; ++iter)
         {
            fn(program.width,
               program.height,
               iter + 1,
               chainPtrs.data(),
               chainWidths.data(),
               chainHeights.data(),
               chainStrides.data(),
               chainOutput.data());
            std::swap(chainInput, chainOutput);
            chainPtrs[0] = chainInput.data();
         }

         output = chainInput;
      }
      else
      {
         fn(program.width,
            program.height,
            1,
            dataPtrs.data(),
            inputWidths.data(),
            inputHeights.data(),
            inputStrides.data(),
            output.data());
      }

      return output;
   }

   std::vector<ParserCase> buildParserCases()
   {
      return {
          {
              "parser_comments_and_whitespace",
              " ( ( A ) ; comment\n   (+ A 1) ) ",
              "((A) (+ A 1))",
          },
          {
              "parser_negative_and_decimal_numbers",
              "((A) (- -1.5 2 3.25))",
              "((A) (- -1.5 2 3.25))",
          },
          {
              "parser_nested_define_structure",
              "((A)\n  (define x (+ A 1))\n  (* x x))",
              "((A) (define x (+ A 1)) (* x x))",
          },
          {
              "parser_absolute_index_symbol",
              "((A) (@A 12 -3))",
              "((A) (@A 12 -3))",
          },
          {
              "parser_iterate_form",
              "(iterate 3 ((A) (+ A 1)))",
              "(iterate 3 ((A) (+ A 1)))",
          },
          {
              "parser_pipeline_form",
              "(pipeline ((A) (+ A 1)) ((A) (* A 2)))",
              "(pipeline ((A) (+ A 1)) ((A) (* A 2)))",
          },
          {
              "parser_lambda_form",
              "((A) ((lambda (x) (+ x 1)) A))",
              "((A) ((lambda (x) (+ x 1)) A))",
          },
      };
   }

   std::vector<ProgramCase> buildCases()
   {
      return {
          {
              "arithmetic_pipeline",
              "((A) (+ (* A 2) 1))",
              2,
              2,
              {{0.0, 0.25, 0.5, 0.75}},
              {1.0, 1.5, 2.0, 2.5},
              0,
              false,
          },
          {
              "define_reuse",
              "((A) (define x (+ A 1)) (* x x))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {1.0, 4.0, 9.0, 16.0},
              0,
              false,
          },
          {
              "comparisons_and_if",
              "((A) (if (> A 0.5) 1 0))",
              2,
              2,
              {{0.25, 0.5, 0.75, 1.0}},
              {0.0, 0.0, 1.0, 1.0},
              0,
              false,
          },
          {
              "min_max_int",
              "((A B) (+ (min A B) (max A B) (int (* 3.7 A))))",
              2,
              2,
              {{0.10, 0.40, 0.70, 0.90},
               {0.30, 0.20, 0.80, 0.10}},
              {0.40, 1.60, 3.50, 4.00},
              0,
              false,
          },
          {
              "relative_index_reflection",
              "((A) (A -1 0))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0}},
              {3.0, 4.0, 1.0, 2.0},
              0,
              false,
          },
          {
              "absolute_index_large_reflection",
              "((A) (@A 0 4))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0}},
              {2.0, 2.0, 2.0, 2.0},
              0,
              false,
          },
          {
              "multi_input_compose",
              "((A B) (* 0.5 (+ A B)))",
              2,
              2,
              {{0.0, 0.2, 0.4, 0.6},
               {0.6, 0.4, 0.2, 0.0}},
              {0.3, 0.3, 0.3, 0.3},
              0,
              false,
          },
          {
              "row_invariant_define",
              "((A) (define row (* i 2)) (+ row j))",
              3,
              2,
              {{0.0, 0.0, 0.0,
                0.0, 0.0, 0.0}},
              {0.0, 1.0, 2.0,
               2.0, 3.0, 4.0},
              0,
              false,
          },
          {
              "fib_rows",
              "((A) (fib i))",
              1,
              4,
              {{0.0, 0.0, 0.0, 0.0}},
              {0.0, 0.0, 1.0, 2.0},
              0,
              false,
          },
          {
              "division_left_associative",
              "((A) (/ 8 2 2))",
              2,
              2,
              {{0.0, 0.0, 0.0, 0.0}},
              {2.0, 2.0, 2.0, 2.0},
              0,
              false,
          },
          {
              "subtraction_left_associative",
              "((A) (- 10 3 2))",
              2,
              2,
              {{0.0, 0.0, 0.0, 0.0}},
              {5.0, 5.0, 5.0, 5.0},
              0,
              false,
          },
          {
              "comparison_operators",
              "((A) (+ (< A 0.5) (<= A 0.5) (> A 0.5) (>= A 0.5) (== A 0.5) (!= A 0.5)))",
              3,
              1,
              {{0.25, 0.50, 0.75}},
              {3.0, 3.0, 3.0},
              0,
              false,
          },
          {
              "abs_operator",
              "((A B) (abs (- A B)))",
              2,
              2,
              {{0.1, 0.9, 0.0, 1.0},
               {0.4, 0.2, 0.5, 0.25}},
              {0.3, 0.7, 0.5, 0.75},
              0,
              false,
          },
          {
              "clamp_operator",
              "((A) (clamp (+ (* 2 A) -0.25) 0.0 1.0))",
              2,
              2,
              {{0.0, 0.2, 0.5, 0.9}},
              {0.0, 0.15, 0.75, 1.0},
              0,
              false,
          },
          {
              "logic_operators",
              "((A) (+ (and (> A 0.2) (< A 0.8)) (or (< A 0.2) (> A 0.8)) (not (== A 0.5))))",
              3,
              1,
              {{0.1, 0.5, 0.9}},
              {2.0, 1.0, 2.0},
              0,
              false,
          },
          {
              "danger_mode_exact_indexing",
              "((A) (A 0 0))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0}},
              {1.0, 2.0, 3.0, 4.0},
              0,
              true,
          },
          {
              "chain_repeat",
              "((A) (+ A 1))",
              2,
              2,
              {{0.0, 0.0, 0.0, 0.0}},
              {3.0, 3.0, 3.0, 3.0},
              3,
              false,
          },
          {
              "iterate_top_level",
              "(iterate 3 ((A) (+ A 1)))",
              2,
              2,
              {{0.0, 0.0, 0.0, 0.0}},
              {3.0, 3.0, 3.0, 3.0},
              0,
              false,
          },
          {
              "iterate_uses_iter_symbol",
              "(iterate 3 ((A) (+ A iter)))",
              2,
              2,
              {{0.0, 0.0, 0.0, 0.0}},
              {6.0, 6.0, 6.0, 6.0},
              0,
              false,
          },
          {
              "iter_symbol_non_chained_defaults_to_one",
              "((A) (+ A iter))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {1.0, 2.0, 3.0, 4.0},
              0,
              false,
          },
          {
              "pipeline_two_stage_pointwise",
              "(pipeline ((A) (+ A 1)) ((A) (* A 2)))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {2.0, 4.0, 6.0, 8.0},
              0,
              false,
          },
          {
              "pipeline_fused_neighbor_access",
              "(pipeline ((A) (+ A 1)) ((A) (+ A (A -1 0))))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {4.0, 6.0, 4.0, 6.0},
              0,
              false,
          },
          {
              "pipeline_preserves_external_inputs",
              "(pipeline ((A B) (+ A B)) ((A B) (* A B)))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0},
               {0.5, 1.0, 1.5, 2.0}},
              {0.75, 3.0, 6.75, 12.0},
              0,
              false,
          },
          {
              "pipeline_previous_and_external_same_stage",
              "(pipeline ((A) (+ A 1)) ((P A) (+ P (@A i j))))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0}},
              {3.0, 5.0, 7.0, 9.0},
              0,
              false,
          },
          {
              "pipeline_stage_defines",
              "(pipeline ((A) (define x (+ A 1)) (* x x)) ((A) (- A 1)))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {0.0, 3.0, 8.0, 15.0},
              0,
              false,
          },
          {
              "lambda_direct_application",
              "((A) ((lambda (x) (+ x 1)) A))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {1.0, 2.0, 3.0, 4.0},
              0,
              false,
          },
          {
              "lambda_named_define",
              "((A) (define inc (lambda (x) (+ x 1))) (inc A))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {1.0, 2.0, 3.0, 4.0},
              0,
              false,
          },
          {
              "lambda_higher_order",
              "((A) ((lambda (f x) (f (f x))) (lambda (x) (+ x 1)) A))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {2.0, 3.0, 4.0, 5.0},
              0,
              false,
          },
          {
              "map_image_lambda",
              "((A) (map-image (lambda (x) (+ (* x 2) 1)) A))",
              2,
              2,
              {{0.0, 0.5, 1.0, 1.5}},
              {1.0, 2.0, 3.0, 4.0},
              0,
              false,
          },
          {
              "zip_image_lambda",
              "((A B) (zip-image (lambda (x y) (- (* x 3) y)) A B))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0},
               {0.5, 1.0, 1.5, 2.0}},
              {2.5, 5.0, 7.5, 10.0},
              0,
              false,
          },
          {
              "zip_image_builtin_symbol",
              "((A B) (zip-image + A B))",
              2,
              2,
              {{1.0, 2.0, 3.0, 4.0},
               {0.5, 1.0, 1.5, 2.0}},
              {1.5, 3.0, 4.5, 6.0},
              0,
              false,
          },
          {
              "lambda_returns_closure_direct_application",
              "((A) (define make-adder (lambda (x) (lambda (y) (+ x y)))) ((make-adder 4) A))",
              2,
              2,
              {{0.0, 1.0, 2.0, 3.0}},
              {4.0, 5.0, 6.0, 7.0},
              0,
              false,
          },
          {
              "lambda_returns_closure_map_image",
              "((A) (define make-bias (lambda (amt) (lambda (px) (+ px amt)))) (map-image (make-bias 0.1) A))",
              2,
              2,
              {{0.0, 0.1, 0.2, 0.3}},
              {0.1, 0.2, 0.3, 0.4},
              0,
              false,
          },
          {
              "lambda_returns_closure_zip_image",
              "((A B) (define make-blend (lambda (w) (lambda (x y) (+ (* w x) (* (- 1 w) y))))) (zip-image (make-blend 0.25) A B))",
              2,
              2,
              {{0.0, 0.2, 0.4, 0.6},
               {1.0, 0.8, 0.6, 0.4}},
              {0.75, 0.65, 0.55, 0.45},
              0,
              false,
          },
          {
              "lambda_returns_closure_with_if_capture",
              "((A) (define make-threshold (lambda (limit) (lambda (x) (if (> x limit) 1 0)))) (map-image (make-threshold 0.5) A))",
              2,
              2,
              {{0.25, 0.5, 0.75, 1.0}},
              {0.0, 0.0, 1.0, 1.0},
              0,
              false,
          },
      };
   }

   int runParserCase(const ParserCase &parserCase)
   {
      try
      {
         gnine::Cell parsed = gnine::cellFromString(parserCase.source);
         std::string printed = gnine::cellToString(parsed);
         if (printed != parserCase.expectedPrinted)
         {
            std::cerr << parserCase.name << ": printed form mismatch.\n"
                      << "expected: " << parserCase.expectedPrinted << "\n"
                      << "actual:   " << printed << "\n";
            return 1;
         }
         std::cout << "[PASS] " << parserCase.name << "\n";
         return 0;
      }
      catch (const std::exception &ex)
      {
         std::cerr << "[FAIL] " << parserCase.name << ": " << ex.what() << "\n";
         return 1;
      }
   }

   int runCase(const ProgramCase &program)
   {
      try
      {
         std::vector<double> actual = runProgram(program);
         if (actual.size() != program.expected.size())
         {
            std::cerr << program.name << ": output size mismatch. expected "
                      << program.expected.size() << " got " << actual.size() << "\n";
            return 1;
         }

         for (size_t idx = 0; idx < actual.size(); ++idx)
         {
            if (!almostEqual(actual[idx], program.expected[idx]))
            {
               std::cerr << program.name << ": mismatch at index " << idx
                         << ". expected " << program.expected[idx]
                         << " got " << actual[idx] << "\n";
               std::cerr << "expected: " << formatVector(program.expected, program.width) << "\n";
               std::cerr << "actual:   " << formatVector(actual, program.width) << "\n";
               return 1;
            }
         }

         std::cout << "[PASS] " << program.name << "\n";
         return 0;
      }
      catch (const std::exception &ex)
      {
         std::cerr << "[FAIL] " << program.name << ": " << ex.what() << "\n";
         return 1;
      }
   }
}

int main(int argc, char *argv[])
{
   std::vector<ParserCase> parserCases = buildParserCases();
   std::vector<ProgramCase> cases = buildCases();

   if (argc == 2 && std::string(argv[1]) == "--list")
   {
      for (const ParserCase &parserCase : parserCases)
         std::cout << parserCase.name << "\n";
      for (const ProgramCase &program : cases)
         std::cout << program.name << "\n";
      return 0;
   }

   std::set<std::string> requested;
   for (int idx = 1; idx < argc; ++idx)
      requested.insert(argv[idx]);

   std::string jitOptions = "-Xjit:acceptHugeMethods,enableBasicBlockHoisting,omitFramePointer,useILValidator";
   if (!initializeJitWithOptions(const_cast<char *>(jitOptions.c_str())))
   {
      std::cerr << "Failed to initialize JIT\n";
      return 1;
   }

   int failures = 0;
   for (const ParserCase &parserCase : parserCases)
   {
      if (!requested.empty() && requested.count(parserCase.name) == 0)
         continue;
      failures += runParserCase(parserCase);
   }

   for (const ProgramCase &program : cases)
   {
      if (!requested.empty() && requested.count(program.name) == 0)
         continue;
      failures += runCase(program);
   }

   shutdownJit();

   if (failures != 0)
      return 1;

   return 0;
}
