#include "Runtime.hpp"

#include "ImageArray.hpp"
#include "VectorProgram.hpp"
#include "JitBuilder.hpp"
#include "compiler/runtime/Runtime.hpp"
#include "compiler/ilgen/IlBuilder.hpp"
#include "compiler/ilgen/MethodBuilder.hpp"
#include "compiler/ilgen/TypeDictionary.hpp"
#include "jitbuilder/compile/ResolvedMethod.hpp"
#include "jitbuilder/ilgen/IlGeneratorMethodDetails.hpp"
#include "compiler/control/CompileMethod.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace gnine
{
   namespace runtime
   {
      namespace
      {
         double parseNumberLiteral(const std::string &literal)
         {
            std::stringstream ss(literal);
            double value = 0.0;
            char trailing = '\0';
            ss >> value;
            if (!ss || (ss >> trailing))
               throw std::runtime_error("Invalid numeric literal: " + literal);
            return value;
         }

         int reflectIndex(int index, int limit)
         {
            if (limit <= 0)
               throw std::runtime_error("Cannot sample from an empty image");

            int maxIndex = limit - 1;
            while (index < 0 || index > maxIndex)
            {
               if (index < 0)
                  index = -index;
               if (index > maxIndex)
                  index = 2 * maxIndex + 1 - index;
            }
            return index;
         }

         int broadcastChannelIndex(const gnine::Image &image, int outputChannel)
         {
            return image.channelCount() == 1 ? 0 : outputChannel;
         }

         int requireCompatibleZipImageChannels(const gnine::Image &lhs,
                                               const gnine::Image &rhs,
                                               const std::string &context)
         {
            if (lhs.width() != rhs.width() || lhs.height() != rhs.height())
               throw std::runtime_error(context + " requires matching image extents");

            if (lhs.channelCount() != rhs.channelCount() &&
                lhs.channelCount() != 1 &&
                rhs.channelCount() != 1)
            {
               throw std::runtime_error(context + " requires matching channel counts or a grayscale input");
            }

            return std::max(lhs.channelCount(), rhs.channelCount());
         }

         struct PixelContextScope
         {
            PixelContextScope(bool &hasPixelContext,
                              int &currentRow,
                              int &currentCol,
                              int &currentChannel,
                              int row,
                              int col,
                              int channel)
               : hasContext(hasPixelContext),
                 rowRef(currentRow),
                 colRef(currentCol),
                 channelRef(currentChannel),
                 previousHasContext(hasPixelContext),
                 previousRow(currentRow),
                 previousCol(currentCol),
                 previousChannel(currentChannel)
            {
               hasContext = true;
               rowRef = row;
               colRef = col;
               channelRef = channel;
            }

            ~PixelContextScope()
            {
               hasContext = previousHasContext;
               rowRef = previousRow;
               colRef = previousCol;
               channelRef = previousChannel;
            }

            bool &hasContext;
            int &rowRef;
            int &colRef;
            int &channelRef;
            bool previousHasContext;
            int previousRow;
            int previousCol;
            int previousChannel;
         };

         Cell makeSymbolCell(const std::string &value)
         {
            return Cell(Cell::Symbol, value);
         }

         Cell makeDefineCell(const std::string &name, const Cell &value)
         {
            Cell defineExpr(Cell::List);
            defineExpr.list.push_back(makeSymbolCell("define"));
            defineExpr.list.push_back(makeSymbolCell(name));
            defineExpr.list.push_back(value);
            return defineExpr;
         }

         Cell doubleToCell(double value)
         {
            std::ostringstream out;
            out << std::setprecision(17) << value;
            return Cell(Cell::Number, out.str());
         }

         bool isValidPattern(const Cell &pattern)
         {
            if (pattern.type == Cell::Symbol)
               return true;
            if (pattern.type != Cell::List)
               return false;
            for (size_t idx = 0; idx < pattern.list.size(); ++idx)
            {
               if (!isValidPattern(pattern.list[idx]))
                  return false;
            }
            return true;
         }

         void collectPatternBoundNames(const Cell &pattern,
                                       std::vector<std::string> *names)
         {
            if (pattern.type == Cell::Symbol)
            {
               names->push_back(pattern.val);
               return;
            }
            if (pattern.type != Cell::List)
               return;
            for (size_t idx = 0; idx < pattern.list.size(); ++idx)
               collectPatternBoundNames(pattern.list[idx], names);
         }

         Cell specializeSymbol(const Cell &expr,
                               const std::string &symbolName,
                               const Cell &replacement)
         {
            if (expr.type == Cell::Symbol)
               return expr.val == symbolName ? replacement : expr;

            if (expr.type != Cell::List)
               return expr;

            if (!expr.list.empty() &&
                expr.list[0].type == Cell::Symbol &&
                expr.list[0].val == "lambda" &&
                expr.list.size() == 3 &&
                expr.list[1].type == Cell::List)
            {
               for (size_t idx = 0; idx < expr.list[1].list.size(); ++idx)
               {
                  if (expr.list[1].list[idx].type == Cell::Symbol &&
                      expr.list[1].list[idx].val == symbolName)
                     return expr;
               }

               Cell specialized(expr);
               specialized.list[2] = specializeSymbol(expr.list[2], symbolName, replacement);
               return specialized;
            }

            Cell specialized(expr);
            for (size_t idx = 0; idx < expr.list.size(); ++idx)
               specialized.list[idx] = specializeSymbol(expr.list[idx], symbolName, replacement);
            return specialized;
         }

         bool findCompiledBinding(EnvironmentObject *env,
                                  const std::string &name,
                                  Value *outValue,
                                  const Cell **outSourceExpr);

         Cell specializeImageMetadataCalls(const Cell &expr,
                                           EnvironmentObject *env)
         {
            if (expr.type != Cell::List || expr.list.empty())
               return expr;

            if (expr.list[0].type == Cell::Symbol &&
                expr.list.size() == 2 &&
                expr.list[1].type == Cell::Symbol &&
                (expr.list[0].val == "width" ||
                 expr.list[0].val == "height" ||
                 expr.list[0].val == "channels"))
            {
               Value value;
               const Cell *sourceExpr = NULL;
               if (findCompiledBinding(env, expr.list[1].val, &value, &sourceExpr) &&
                   value.isObject() &&
                   value.object->type == Object::Image)
               {
                  const ImageObject *image = static_cast<const ImageObject *>(value.object);
                  if (expr.list[0].val == "width")
                     return doubleToCell(static_cast<double>(image->image->width()));
                  if (expr.list[0].val == "height")
                     return doubleToCell(static_cast<double>(image->image->height()));
                  return doubleToCell(static_cast<double>(image->image->channelCount()));
               }
            }

            if (!expr.list.empty() &&
                expr.list[0].type == Cell::Symbol &&
                expr.list[0].val == "lambda" &&
                expr.list.size() == 3 &&
                expr.list[1].type == Cell::List)
            {
               Cell specialized(expr);
               specialized.list[2] = specializeImageMetadataCalls(expr.list[2], env);
               return specialized;
            }

            Cell specialized(expr);
            for (size_t idx = 0; idx < expr.list.size(); ++idx)
               specialized.list[idx] = specializeImageMetadataCalls(expr.list[idx], env);
            return specialized;
         }

         Cell renameBindingRefs(const Cell &expr,
                                const std::string &from,
                                const std::string &to)
         {
            if (expr.type == Cell::Symbol)
               return expr.val == from ? makeSymbolCell(to) : expr;

            if (expr.type != Cell::List)
               return expr;

            if (!expr.list.empty() &&
                expr.list[0].type == Cell::Symbol &&
                expr.list[0].val == "lambda" &&
                expr.list.size() == 3 &&
                expr.list[1].type == Cell::List)
            {
               for (size_t idx = 0; idx < expr.list[1].list.size(); ++idx)
               {
                  if (expr.list[1].list[idx].type == Cell::Symbol &&
                      expr.list[1].list[idx].val == from)
                     return expr;
               }

               Cell renamed(expr);
               renamed.list[2] = renameBindingRefs(expr.list[2], from, to);
               return renamed;
            }

            Cell renamed(expr);
            for (size_t idx = 0; idx < expr.list.size(); ++idx)
            {
               if (idx == 0 &&
                   expr.list[idx].type == Cell::Symbol &&
                   expr.list[idx].val == from)
               {
                  renamed.list[idx] = makeSymbolCell(to);
               }
               else
               {
                  renamed.list[idx] = renameBindingRefs(expr.list[idx], from, to);
               }
            }
            return renamed;
         }

         std::string argumentBindingName(const Cell &pattern, size_t index)
         {
            if (pattern.type == Cell::Symbol)
               return pattern.val;
            return "__arg" + std::to_string(index) + "__";
         }

         Cell tupleAccessExpr(const Cell &baseExpr, size_t index)
         {
            Cell expr(Cell::List);
            expr.list.push_back(makeSymbolCell("get"));
            expr.list.push_back(baseExpr);
            expr.list.push_back(doubleToCell(static_cast<double>(index)));
            return expr;
         }

         bool ensureRuntimeJitInitialized()
         {
            static bool initialized = false;
            if (initialized)
               return true;

            static char jitOptions[] = "-Xjit:acceptHugeMethods,enableBasicBlockHoisting,omitFramePointer,useILValidator";
            initialized = initializeJitWithOptions(jitOptions);
            return initialized;
         }

         bool isCompiledBuiltinName(const std::string &name)
         {
            return name == "+" || name == "-" || name == "*" || name == "/" ||
                   name == "<" || name == "<=" || name == ">" || name == ">=" ||
                   name == "==" || name == "!=" || name == "and" || name == "or" ||
                   name == "not" || name == "min" || name == "max" || name == "abs" ||
                   name == "clamp" || name == "int" ||
                   name == "width" || name == "height" || name == "channels" ||
                   name == "if" || name == "lambda" || name == "map-image" ||
                   name == "zip-image" || name == "canvas" ||
                   name == "mod" || name == "floor" || name == "ceil" ||
                   name == "sqrt" || name == "sin" || name == "cos" ||
                   name == "pow" || name == "atan2" || name == "rand-of" ||
                   name == "draw-line" ||
                   name == "cond" || name == "let" || name == "let*" ||
                   name == "begin" || name == "rgb" || name == "vec";
         }

         struct ScalarInputBinding
         {
            std::string name;
            double value;
         };

        struct CompiledRuntimeKernel
        {
            ImageArrayFunctionType *fn;
        };

         struct CompiledRuntimeRGBKernel
         {
            ImageArrayRGBFunctionType *fn;
         };

        struct CompiledKernelLookup
        {
            bool ok;
            bool cacheHit;
            double compileMillis;
            std::string fallbackReason;
            ImageArrayFunctionType *fn;

            CompiledKernelLookup()
               : ok(false), cacheHit(false), compileMillis(0.0), fn(NULL)
            {
            }
        };

         struct CompiledRGBKernelLookup
         {
            bool ok;
            bool cacheHit;
            double compileMillis;
            std::string fallbackReason;
            ImageArrayRGBFunctionType *fn;

            CompiledRGBKernelLookup()
               : ok(false), cacheHit(false), compileMillis(0.0), fn(NULL)
            {
            }
         };

        std::map<std::string, CompiledRuntimeKernel> &runtimeKernelCache()
        {
            static std::map<std::string, CompiledRuntimeKernel> cache;
            return cache;
        }

         std::map<std::string, CompiledRuntimeRGBKernel> &runtimeRGBKernelCache()
         {
            static std::map<std::string, CompiledRuntimeRGBKernel> cache;
            return cache;
         }

         double elapsedMillis(const std::chrono::steady_clock::time_point &start,
                              const std::chrono::steady_clock::time_point &end)
         {
            return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
         }

         std::string formatMillis(double millis)
         {
            std::ostringstream out;
            out << std::fixed << std::setprecision(3) << millis;
            return out.str();
         }

         std::string summarizeProgram(const Cell &program, size_t maxChars)
         {
            std::string text = cellToString(program);
            if (text.size() <= maxChars)
               return text;
            return text.substr(0, maxChars) + "...";
         }

         std::string dumpRuntimeProgram(const Cell &program)
         {
            static int dumpCounter = 0;
            const std::string path = "/tmp/gnine_runtime_jit_fail_" + std::to_string(++dumpCounter) + ".psm";
            std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
            out << cellToString(program) << std::endl;
            return path;
         }

         int32_t compileRuntimeMethodBuilderCold(OMR::JitBuilder::MethodBuilder *methodBuilder,
                                                 void **entry)
         {
            TR::MethodBuilder *impl =
                static_cast<TR::MethodBuilder *>(
                    static_cast<TR::IlBuilder *>(methodBuilder != NULL ? methodBuilder->_impl : NULL));
            if (impl == NULL || entry == NULL)
               return -1;

            TR::ResolvedMethod resolvedMethod(impl);
            TR::IlGeneratorMethodDetails details(&resolvedMethod);

            int32_t rc = 0;
            *entry = reinterpret_cast<void *>(compileMethodFromDetails(NULL, details, cold, rc));
            impl->typeDictionary()->NotifyCompilationDone();
            return rc;
         }

         bool lookupOrCompileRuntimeKernel(const Cell &program,
                                           CompiledKernelLookup *result)
        {
            LoweredProgram lowered = lowerProgram(program);
            if (lowered.usesVectorFeatures || lowered.channelPrograms.size() != 1)
            {
               result->fallbackReason = lowered.usesVectorFeatures ? "vector_features" : "lowered_shape";
               return false;
            }

            const std::string cacheKey = cellToString(program);
            std::map<std::string, CompiledRuntimeKernel> &cache = runtimeKernelCache();
            std::map<std::string, CompiledRuntimeKernel>::const_iterator it = cache.find(cacheKey);
            if (it != cache.end())
            {
               result->ok = true;
               result->cacheHit = true;
               result->fn = it->second.fn;
               return true;
            }

            OMR::JitBuilder::TypeDictionary types;
            ImageArray method(&types);
            method.runByteCodes(lowered.channelPrograms[0], false);

            void *entry = NULL;
            std::chrono::steady_clock::time_point compileStart = std::chrono::steady_clock::now();
            int32_t rc = compileRuntimeMethodBuilderCold(&method, &entry);
            std::chrono::steady_clock::time_point compileEnd = std::chrono::steady_clock::now();
            if (rc != 0)
            {
               const std::string dumpPath = dumpRuntimeProgram(program);
               result->fallbackReason = "jit_compile_failed rc=" + std::to_string(rc) +
                                        " dump=" + dumpPath +
                                        " program=" + summarizeProgram(program, 800);
               return false;
            }

            result->compileMillis = elapsedMillis(compileStart, compileEnd);
            result->ok = true;
            result->cacheHit = false;
            result->fn = reinterpret_cast<ImageArrayFunctionType *>(entry);
           cache[cacheKey].fn = result->fn;
           return true;
        }

         bool lookupOrCompileRuntimeRGBKernel(const Cell &program,
                                              CompiledRGBKernelLookup *result)
         {
            const std::string cacheKey = cellToString(program);
            std::map<std::string, CompiledRuntimeRGBKernel> &cache = runtimeRGBKernelCache();
            std::map<std::string, CompiledRuntimeRGBKernel>::const_iterator it = cache.find(cacheKey);
            if (it != cache.end())
            {
               result->ok = true;
               result->cacheHit = true;
               result->fn = it->second.fn;
               return true;
            }

            std::chrono::steady_clock::time_point compileStart = std::chrono::steady_clock::now();
            OMR::JitBuilder::TypeDictionary types;
            ImageArray method(&types, 3);
            method.runByteCodes(program, false);

            void *entry = 0;
            int32_t rc = compileRuntimeMethodBuilderCold(&method, &entry);
            std::chrono::steady_clock::time_point compileEnd = std::chrono::steady_clock::now();
            if (rc != 0)
            {
               std::string dumpPath = dumpRuntimeProgram(program);
               result->fallbackReason = "jit_compile_failed rc=" + std::to_string(rc) +
                                        " dump=" + dumpPath +
                                        " program=" + summarizeProgram(program, 800);
               return false;
            }

            result->compileMillis = elapsedMillis(compileStart, compileEnd);
            result->ok = true;
            result->cacheHit = false;
            result->fn = reinterpret_cast<ImageArrayRGBFunctionType *>(entry);
            cache[cacheKey].fn = result->fn;
            return true;
         }

         int runtimeIterValue(EnvironmentObject *env)
         {
            for (EnvironmentObject *current = env; current != NULL; current = current->parent)
            {
               auto it = current->bindings.find("iter");
               if (it != current->bindings.end() && it->second.isNumber())
                  return static_cast<int>(it->second.number);
            }
            return 1;
         }

         Cell makeLambdaCell(const ClosureObject *closure)
         {
            Cell params(Cell::List);
            for (size_t idx = 0; idx < closure->paramPatterns.size(); ++idx)
               params.list.push_back(closure->paramPatterns[idx]);

            Cell lambdaExpr(Cell::List);
            lambdaExpr.list.push_back(makeSymbolCell("lambda"));
            lambdaExpr.list.push_back(params);
            lambdaExpr.list.push_back(closure->body);
            return lambdaExpr;
         }

         Cell rewriteCompiledScalarExpr(const Cell &expr)
         {
            if (expr.type != Cell::List || expr.list.empty())
               return expr;

            if (expr.list[0].type == Cell::Symbol &&
                expr.list.size() == 2 &&
                expr.list[1].type == Cell::Symbol)
            {
               if (expr.list[0].val == "width")
                  return makeSymbolCell("width");
               if (expr.list[0].val == "height")
                  return makeSymbolCell("height");
            }

            Cell rewritten(Cell::List);
            rewritten.list.reserve(expr.list.size());
            for (size_t idx = 0; idx < expr.list.size(); ++idx)
               rewritten.list.push_back(rewriteCompiledScalarExpr(expr.list[idx]));
            return rewritten;
         }

         bool containsMetadataBuiltinCall(const Cell &expr)
         {
            if (expr.type != Cell::List || expr.list.empty())
               return false;

            if (expr.list[0].type == Cell::Symbol &&
                (expr.list[0].val == "width" || expr.list[0].val == "height" || expr.list[0].val == "channels"))
               return true;

            for (size_t idx = 0; idx < expr.list.size(); ++idx)
            {
               if (containsMetadataBuiltinCall(expr.list[idx]))
                  return true;
            }
            return false;
         }

         bool isIntrinsicCompiledScalarSymbol(const std::string &name)
         {
            return name == "iter" || name == "i" || name == "j" || name == "c" ||
                   name == "width" || name == "height";
         }

         bool isLowerableCompiledScalarExpr(const Cell &expr)
         {
            if (expr.type == Cell::Number || expr.type == Cell::Symbol)
               return true;
            if (expr.type != Cell::List || expr.list.empty() || expr.list[0].type != Cell::Symbol)
               return false;

            const std::string &head = expr.list[0].val;
            if (head == "get" || head == "tuple" || head == "lambda" ||
                head == "map-image" || head == "zip-image" || head == "canvas")
               return false;

            for (size_t idx = 1; idx < expr.list.size(); ++idx)
            {
               if (!isLowerableCompiledScalarExpr(expr.list[idx]))
                  return false;
            }
            return true;
         }

         bool bindingHasDynamicCompiledSource(EnvironmentObject *env,
                                              const std::string &name,
                                              std::set<std::string> *visited);

         bool exprHasOnlyDynamicCompiledSymbols(const Cell &expr,
                                                EnvironmentObject *env,
                                                std::set<std::string> *visited)
         {
            if (expr.type == Cell::Number)
               return true;
            if (expr.type == Cell::Symbol)
            {
               if (isIntrinsicCompiledScalarSymbol(expr.val))
                  return true;
               return bindingHasDynamicCompiledSource(env, expr.val, visited);
            }
            if (expr.type != Cell::List || expr.list.empty())
               return false;
            for (size_t idx = 1; idx < expr.list.size(); ++idx)
            {
               if (!exprHasOnlyDynamicCompiledSymbols(expr.list[idx], env, visited))
                  return false;
            }
            return true;
         }

         bool bindingHasDynamicCompiledSource(EnvironmentObject *env,
                                              const std::string &name,
                                              std::set<std::string> *visited)
         {
            if (!visited->insert(name).second)
               return true;

            for (EnvironmentObject *current = env; current != NULL; current = current->parent)
            {
               auto sourceIt = current->sourceExprs.find(name);
               if (sourceIt != current->sourceExprs.end())
               {
                  if (!isLowerableCompiledScalarExpr(sourceIt->second))
                  {
                     auto bindingIt = current->bindings.find(name);
                     return bindingIt != current->bindings.end() && bindingIt->second.isNumber();
                  }
                  return exprHasOnlyDynamicCompiledSymbols(sourceIt->second, env, visited);
               }

               auto bindingIt = current->bindings.find(name);
               if (bindingIt != current->bindings.end())
               {
                  if (bindingIt->second.isNumber())
                     return true;
                  if (bindingIt->second.isExpr())
                     return isLowerableCompiledScalarExpr(bindingIt->second.expr);
                  return false;
               }
            }

            return false;
         }

         bool shouldEmitDynamicCompiledSource(const Cell &expr,
                                              EnvironmentObject *env)
         {
            if (!isLowerableCompiledScalarExpr(expr))
               return false;
            std::set<std::string> visited;
            return exprHasOnlyDynamicCompiledSymbols(expr, env, &visited);
         }

         void collectReferencedSymbols(const Cell &expr,
                                       std::set<std::string> *symbols)
         {
            if (expr.type == Cell::Symbol)
            {
               if (!isCompiledBuiltinName(expr.val) &&
                   !isIntrinsicCompiledScalarSymbol(expr.val))
                  symbols->insert(expr.val);
               return;
            }

            if (expr.type != Cell::List)
               return;

            if (!expr.list.empty() &&
                expr.list[0].type == Cell::Symbol &&
                expr.list[0].val == "lambda" &&
                expr.list.size() == 3 &&
                expr.list[1].type == Cell::List)
            {
               std::set<std::string> nested;
               collectReferencedSymbols(expr.list[2], &nested);
               for (size_t idx = 0; idx < expr.list[1].list.size(); ++idx)
               {
                  if (expr.list[1].list[idx].type == Cell::Symbol)
                     nested.erase(expr.list[1].list[idx].val);
               }
               symbols->insert(nested.begin(), nested.end());
               return;
            }

            for (size_t idx = 0; idx < expr.list.size(); ++idx)
               collectReferencedSymbols(expr.list[idx], symbols);
         }

         bool findCompiledBinding(EnvironmentObject *env,
                                  const std::string &name,
                                  Value *outValue,
                                  const Cell **outSourceExpr)
         {
            for (EnvironmentObject *current = env; current != NULL; current = current->parent)
            {
               auto bindingIt = current->bindings.find(name);
               if (bindingIt == current->bindings.end())
                  continue;
               *outValue = bindingIt->second;
               auto sourceIt = current->sourceExprs.find(name);
               *outSourceExpr = sourceIt == current->sourceExprs.end() ? NULL : &sourceIt->second;
               return true;
            }
            return false;
         }

         bool appendCompiledBinding(const std::string &name,
                                    const Value &value,
                                    const Cell *sourceExpr,
                                    EnvironmentObject *lookupEnv,
                                    const std::set<std::string> &excludedNames,
                                    std::set<std::string> *emittedNames,
                                    std::vector<ScalarInputBinding> *scalarInputs,
                                    std::vector<const gnine::Image *> *inputImages,
                                    bool allowDynamicScalarRewrite,
                                    Cell *argsCell,
                                    Cell *program);

         bool appendCompiledReferencedBindings(EnvironmentObject *env,
                                              const Cell &expr,
                                              const std::vector<std::string> &symbols,
                                              const std::set<std::string> &excludedNames,
                                              std::set<std::string> *emittedNames,
                                              std::vector<ScalarInputBinding> *scalarInputs,
                                              std::vector<const gnine::Image *> *inputImages,
                                              bool allowDynamicScalarRewrite,
                                              Cell *argsCell,
                                              Cell *program)
         {
            for (size_t idx = 0; idx < symbols.size(); ++idx)
            {
               Value value;
               const Cell *sourceExpr = NULL;
               if (!findCompiledBinding(env, symbols[idx], &value, &sourceExpr))
                  continue;
               if (!appendCompiledBinding(symbols[idx], value, sourceExpr, env, excludedNames,
                                          emittedNames, scalarInputs, inputImages,
                                          allowDynamicScalarRewrite, argsCell, program))
                  return false;
            }
            return true;
         }

         bool appendCompiledReferencedBindings(EnvironmentObject *env,
                                              const Cell &expr,
                                              const std::set<std::string> &symbols,
                                              const std::set<std::string> &excludedNames,
                                              std::set<std::string> *emittedNames,
                                              std::vector<ScalarInputBinding> *scalarInputs,
                                              std::vector<const gnine::Image *> *inputImages,
                                              bool allowDynamicScalarRewrite,
                                              Cell *argsCell,
                                              Cell *program)
         {
            for (std::set<std::string>::const_iterator it = symbols.begin(); it != symbols.end(); ++it)
            {
               Value value;
               const Cell *sourceExpr = NULL;
               if (!findCompiledBinding(env, *it, &value, &sourceExpr))
                  continue;
               if (!appendCompiledBinding(*it, value, sourceExpr, env, excludedNames,
                                          emittedNames, scalarInputs, inputImages,
                                          allowDynamicScalarRewrite, argsCell, program))
                  return false;
            }
            return true;
         }

         bool appendCompiledReferencedBindings(EnvironmentObject *env,
                                              const Cell &expr,
                                              const std::set<std::string> &excludedNames,
                                              std::set<std::string> *emittedNames,
                                              std::vector<ScalarInputBinding> *scalarInputs,
                                              std::vector<const gnine::Image *> *inputImages,
                                              bool allowDynamicScalarRewrite,
                                               Cell *argsCell,
                                               Cell *program)
         {
            std::set<std::string> symbols;
            collectReferencedSymbols(expr, &symbols);
            return appendCompiledReferencedBindings(env, expr, symbols, excludedNames, emittedNames,
                                                   scalarInputs, inputImages, allowDynamicScalarRewrite,
                                                   argsCell, program);
         }

         bool appendCompiledBinding(const std::string &name,
                                    const Value &value,
                                    const Cell *sourceExpr,
                                    EnvironmentObject *lookupEnv,
                                    const std::set<std::string> &excludedNames,
                                    std::set<std::string> *emittedNames,
                                    std::vector<ScalarInputBinding> *scalarInputs,
                                    std::vector<const gnine::Image *> *inputImages,
                                    bool allowDynamicScalarRewrite,
                                    Cell *argsCell,
                                    Cell *program)
         {
            if (excludedNames.count(name) > 0 || !emittedNames->insert(name).second)
               return true;

            if (value.isNumber())
            {
               Cell emittedExpr = doubleToCell(value.number);
               if (allowDynamicScalarRewrite &&
                   sourceExpr != NULL &&
                   shouldEmitDynamicCompiledSource(*sourceExpr, lookupEnv))
               {
                  if (!appendCompiledReferencedBindings(lookupEnv, *sourceExpr, excludedNames, emittedNames,
                                                       scalarInputs, inputImages, allowDynamicScalarRewrite,
                                                       argsCell, program))
                     return false;
                  emittedExpr = rewriteCompiledScalarExpr(*sourceExpr);
               }
               else if (sourceExpr != NULL)
               {
                  argsCell->list.push_back(makeSymbolCell(name));
                  scalarInputs->push_back(ScalarInputBinding{name, value.number});
                  return true;
               }
               program->list.push_back(makeDefineCell(name, emittedExpr));
               return true;
            }

            if (value.isExpr())
            {
               program->list.push_back(makeDefineCell(name, value.expr));
               return true;
            }

            if (value.isBuiltin())
               return true;

            if (value.isObject() && value.object->type == Object::Image)
            {
               argsCell->list.push_back(makeSymbolCell(name));
               inputImages->push_back(static_cast<ImageObject *>(value.object)->image);
               return true;
            }

            if (value.isObject() && value.object->type == Object::Closure)
            {
               const ClosureObject *closure = static_cast<ClosureObject *>(value.object);
               std::set<std::string> nestedExcluded = excludedNames;
               std::vector<std::string> boundNames;
               for (size_t idx = 0; idx < closure->paramPatterns.size(); ++idx)
                  collectPatternBoundNames(closure->paramPatterns[idx], &boundNames);
               nestedExcluded.insert(boundNames.begin(), boundNames.end());
               if (!appendCompiledReferencedBindings(closure->env, closure->body, nestedExcluded,
                                                    emittedNames, scalarInputs, inputImages, allowDynamicScalarRewrite,
                                                    argsCell, program))
                  return false;
               program->list.push_back(makeDefineCell(name, makeLambdaCell(closure)));
               return true;
            }

            return false;
         }
      }

      Value::Value()
         : kind(Nil), number(0.0), hasSourceExpr(false), object(NULL)
      {
      }

      Value Value::nil()
      {
         return Value();
      }

      Value Value::numberValue(double value)
      {
         Value result;
         result.kind = Number;
         result.number = value;
         return result;
      }

      Value Value::numberValue(double value, const Cell &sourceExpr)
      {
         Value result = numberValue(value);
         result.hasSourceExpr = true;
         result.sourceExpr = sourceExpr;
         return result;
      }

      Value Value::exprValue(const Cell &expr)
      {
         Value result;
         result.kind = Expr;
         result.expr = expr;
         return result;
      }

      Value Value::builtinValue(const std::string &name)
      {
         Value result;
         result.kind = Builtin;
         result.builtinName = name;
         return result;
      }

      Value Value::objectValue(Object *value)
      {
         Value result;
         result.kind = ObjectRef;
         result.object = value;
         return result;
      }

      bool Value::isNil() const
      {
         return kind == Nil;
      }

      bool Value::isNumber() const
      {
         return kind == Number;
      }

      bool Value::isExpr() const
      {
         return kind == Expr;
      }

      bool Value::isBuiltin() const
      {
         return kind == Builtin;
      }

      bool Value::isObject() const
      {
         return kind == ObjectRef;
      }

      bool Value::hasNumberSourceExpr() const
      {
         return kind == Number && hasSourceExpr;
      }

      Object::Object(Type objectType)
         : type(objectType), marked(false), next(NULL)
      {
      }

      Object::~Object()
      {
      }

      EnvironmentObject::EnvironmentObject(EnvironmentObject *parentEnv)
         : Object(Environment), parent(parentEnv)
      {
      }

      void EnvironmentObject::trace(Heap &heap)
      {
         heap.markObject(parent);
         for (auto it = bindings.begin();
              it != bindings.end();
              ++it)
            heap.markValue(it->second);
      }

      ClosureObject::ClosureObject(const std::vector<Cell> &patterns,
                                   const std::vector<std::string> &parameters,
                                   const Cell &bodyExpr,
                                   EnvironmentObject *capturedEnv)
         : Object(Closure), paramPatterns(patterns), params(parameters), body(bodyExpr), env(capturedEnv)
      {
      }

      void ClosureObject::trace(Heap &heap)
      {
         heap.markObject(env);
      }

      ImageObject::ImageObject(const gnine::Image &source)
         : Object(Image), image(new gnine::Image(source.width(), source.height(), source.stride(), source.channelCount()))
      {
         std::copy(source.getData(),
                   source.getData() + source.channelCount() * source.planeSize(),
                   image->getData());
      }

      ImageObject::ImageObject(gnine::Image &&source)
         : Object(Image), image(new gnine::Image(std::move(source)))
      {
      }

      ImageObject::~ImageObject()
      {
         delete image;
      }

      void ImageObject::trace(Heap &)
      {
      }

      TupleObject::TupleObject(const std::vector<Value> &elements)
         : Object(Tuple), values(elements)
      {
      }

      void TupleObject::trace(Heap &heap)
      {
         for (size_t idx = 0; idx < values.size(); ++idx)
            heap.markValue(values[idx]);
      }

      Heap::Root::Root(Heap &heap, Value &slot)
         : _heap(&heap), _slot(&slot)
      {
         _heap->addRoot(_slot);
      }

      Heap::Root::~Root()
      {
         _heap->removeRoot(_slot);
      }

      Heap::Heap()
         : _objects(NULL),
           _liveObjects(0),
           _collectionCount(0),
           _nextCollectionThreshold(16)
      {
      }

      Heap::~Heap()
      {
         Object *object = _objects;
         while (object != NULL)
         {
            Object *next = object->next;
            delete object;
            object = next;
         }
      }

      EnvironmentObject *Heap::allocateEnvironment(EnvironmentObject *parent)
      {
         return allocateObject<EnvironmentObject>(parent);
      }

      ClosureObject *Heap::allocateClosure(const std::vector<std::string> &params,
                                           const std::vector<Cell> &patterns,
                                           const Cell &body,
                                           EnvironmentObject *env)
      {
         return allocateObject<ClosureObject>(patterns, params, body, env);
      }

      ImageObject *Heap::allocateImage(const gnine::Image &image)
      {
         return allocateObject<ImageObject>(image);
      }

      ImageObject *Heap::allocateImage(gnine::Image &&image)
      {
         return allocateObject<ImageObject>(std::move(image));
      }

      TupleObject *Heap::allocateTuple(const std::vector<Value> &values)
      {
         return allocateObject<TupleObject>(values);
      }

      void Heap::addRoot(Value *slot)
      {
         _roots.push_back(slot);
      }

      void Heap::removeRoot(Value *slot)
      {
         std::vector<Value *>::iterator it = std::find(_roots.begin(), _roots.end(), slot);
         if (it == _roots.end())
            throw std::runtime_error("Attempted to remove unknown GC root");
         _roots.erase(it);
      }

      void Heap::collect()
      {
         ++_collectionCount;
         for (size_t idx = 0; idx < _roots.size(); ++idx)
            markValue(*_roots[idx]);
         sweep();
         _nextCollectionThreshold = std::max(static_cast<size_t>(16), _liveObjects * 2);
      }

      void Heap::markValue(const Value &value)
      {
         if (value.isObject())
            markObject(value.object);
      }

      void Heap::markObject(Object *object)
      {
         if (object == NULL || object->marked)
            return;
         object->marked = true;
         object->trace(*this);
      }

      size_t Heap::liveObjectCount() const
      {
         return _liveObjects;
      }

      size_t Heap::collectionCount() const
      {
         return _collectionCount;
      }

      template <typename T, typename... Args>
      T *Heap::allocateObject(Args&&... args)
      {
         if (_liveObjects >= _nextCollectionThreshold)
            collect();

         T *object = new T(std::forward<Args>(args)...);
         object->next = _objects;
         _objects = object;
         ++_liveObjects;
         return object;
      }

      void Heap::sweep()
      {
         Object **link = &_objects;
         while (*link != NULL)
         {
            Object *object = *link;
            if (!object->marked)
            {
               *link = object->next;
               delete object;
               --_liveObjects;
            }
            else
            {
               object->marked = false;
               link = &object->next;
            }
         }
      }

      Evaluator::Evaluator()
         : _hasPixelContext(false),
           _currentRow(0),
           _currentCol(0),
           _currentChannel(0),
           _reportedNonNumericPixel(false)
      {
      }

      const Evaluator::ProgramMetadata &Evaluator::programMetadata(const Cell &program)
      {
         const std::string cacheKey = cellToString(program);
         std::map<std::string, ProgramMetadata>::const_iterator cached = _programMetadataCache.find(cacheKey);
         if (cached != _programMetadataCache.end())
            return cached->second;

         if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
            throw std::runtime_error("Runtime program must be of form (() expr)");

         const Cell &argsCell = program.list[0];
         ProgramMetadata metadata;
         metadata.argBindingNames.reserve(argsCell.list.size());
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         {
            if (!isValidPattern(argsCell.list[idx]))
               throw std::runtime_error("Runtime program arguments must be symbols or tuple patterns");
            metadata.argBindingNames.push_back(argumentBindingName(argsCell.list[idx], idx));
         }

         bool sawResult = false;
         for (size_t idx = 1; idx < program.list.size(); ++idx)
         {
            const Cell &expr = program.list[idx];
            bool isDefine = expr.type == Cell::List &&
                            expr.list.size() == 3 &&
                            expr.list[0].type == Cell::Symbol &&
                            expr.list[0].val == "define" &&
                            expr.list[1].type == Cell::Symbol;

            if (isDefine)
            {
               ProgramStatement statement;
               statement.expr = expr;
               statement.defineName = expr.list[1].val;
               statement.isDefine = true;
               metadata.statements.push_back(statement);
               continue;
            }

            if (sawResult)
               throw std::runtime_error("Runtime evaluator expects a single result expression plus defines");

            ProgramStatement statement;
            statement.expr = expr;
            statement.isDefine = false;
            metadata.statements.push_back(statement);
            sawResult = true;
         }

         if (!sawResult)
            throw std::runtime_error("Runtime program must contain a result expression");

         return _programMetadataCache.insert(std::make_pair(cacheKey, metadata)).first->second;
      }

      const Evaluator::CompiledCanvasChannelMetadata &
      Evaluator::compiledCanvasChannelMetadata(const Cell &body, int channel)
      {
         std::pair<std::string, int> key(cellToString(body), channel);
         std::map<std::pair<std::string, int>, CompiledCanvasChannelMetadata>::const_iterator cached =
             _compiledCanvasChannelMetadataCache.find(key);
         if (cached != _compiledCanvasChannelMetadataCache.end())
            return cached->second;

         CompiledCanvasChannelMetadata metadata;
         metadata.specializedBody = specializeSymbol(body, "c", doubleToCell(static_cast<double>(channel)));
         std::set<std::string> referencedSymbols;
         collectReferencedSymbols(metadata.specializedBody, &referencedSymbols);
         metadata.referencedSymbols.assign(referencedSymbols.begin(), referencedSymbols.end());
         return _compiledCanvasChannelMetadataCache.insert(std::make_pair(key, metadata)).first->second;
      }

      Value Evaluator::evaluateProgram(const Cell &program)
      {
         std::map<std::string, Value> bindings;
         return evaluateProgram(program, bindings);
      }

      Value Evaluator::evaluateProgram(const Cell &program, const std::map<std::string, Value> &bindings)
      {
         return evaluateProgram(program, bindings, NULL);
      }

      Value Evaluator::evaluateProgram(const Cell &program,
                                       const std::map<std::string, Value> &bindings,
                                       std::map<std::string, Value> *outBindings)
      {
         const ProgramMetadata &metadata = programMetadata(program);
         const Cell &argsCell = program.list[0];

         std::vector<Value> rootedBindings;
         rootedBindings.reserve(bindings.size());
         for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
            rootedBindings.push_back(it->second);
         for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
            _heap.addRoot(&rootedBindings[idx]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);
         Value symbolicEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root symbolicRoot(_heap, symbolicEnvValue);
         EnvironmentObject *symbolicEnv = static_cast<EnvironmentObject *>(symbolicEnvValue.object);

         try
         {
            for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
            {
               if (std::find(metadata.argBindingNames.begin(),
                             metadata.argBindingNames.end(),
                             it->first) != metadata.argBindingNames.end())
                  continue;
               globalEnv->bindings[it->first] = it->second;
               if (it->second.isNumber())
                  symbolicEnv->bindings[it->first] = Value::exprValue(makeSymbolCell(it->first));
               else
                  symbolicEnv->bindings[it->first] = it->second;
            }

            for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
            {
               const std::string &bindingName = metadata.argBindingNames[idx];
               std::map<std::string, Value>::const_iterator it = bindings.find(bindingName);
               if (it == bindings.end())
                  throw std::runtime_error("Missing runtime binding for program argument: " + bindingName);
               Value symbolicValue = Value::exprValue(makeSymbolCell(bindingName));
               bindPattern(globalEnv, argsCell.list[idx], it->second);
               bindPattern(globalEnv, argsCell.list[idx], it->second, symbolicValue, symbolicEnv);
            }

            Value result = Value::nil();
            Heap::Root resultRoot(_heap, result);
            for (size_t idx = 0; idx < metadata.statements.size(); ++idx)
            {
               const ProgramStatement &statement = metadata.statements[idx];
               const Cell &expr = statement.expr;
               if (statement.isDefine)
               {
                  Value defineValue = eval(expr.list[2], globalEnv);
                  Heap::Root defineRoot(_heap, defineValue);
                  globalEnv->bindings[statement.defineName] = defineValue;
                  globalEnv->sourceExprs.erase(statement.defineName);
                  if (defineValue.isNumber())
                  {
                     Value symbolicValue = eval(expr.list[2], symbolicEnv);
                     Heap::Root symbolicDefineRoot(_heap, symbolicValue);
                     Cell sourceExpr = requireExpr(symbolicValue, "define");
                     if (!containsMetadataBuiltinCall(sourceExpr))
                        globalEnv->sourceExprs[statement.defineName] = sourceExpr;
                     symbolicEnv->bindings[statement.defineName] = Value::exprValue(expr.list[1]);
                  }
                  else
                     symbolicEnv->bindings[statement.defineName] = defineValue;
                  continue;
               }

               result = eval(expr, globalEnv);
            }

            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            if (outBindings != NULL)
               outBindings->insert(globalEnv->bindings.begin(), globalEnv->bindings.end());
            return result;
         }
         catch (...)
         {
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            throw;
         }
      }

      Value Evaluator::evaluateExpr(const Cell &expr)
      {
         std::map<std::string, Value> bindings;
         return evaluateExpr(expr, bindings);
      }

      Value Evaluator::evaluateExpr(const Cell &expr, const std::map<std::string, Value> &bindings)
      {
         std::vector<Value> rootedBindings;
         rootedBindings.reserve(bindings.size());
         for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
            rootedBindings.push_back(it->second);
         for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
            _heap.addRoot(&rootedBindings[idx]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);
         try
         {
            for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
               globalEnv->bindings[it->first] = it->second;
            Value result = eval(expr, globalEnv);
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            return result;
         }
         catch (...)
         {
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            throw;
         }
      }

      Cell Evaluator::normalizeProgram(const Cell &program)
      {
         const ProgramMetadata &metadata = programMetadata(program);
         Cell normalized(Cell::List);
         normalized.list.push_back(program.list[0]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);

         const Cell &argsCell = program.list[0];
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         {
            bindPattern(globalEnv,
                        argsCell.list[idx],
                        Value::exprValue(makeSymbolCell(metadata.argBindingNames[idx])));
         }

         for (size_t idx = 0; idx < metadata.statements.size(); ++idx)
         {
            const ProgramStatement &statement = metadata.statements[idx];
            const Cell &expr = statement.expr;

            if (statement.isDefine)
            {
               Value value = eval(expr.list[2], globalEnv);
               globalEnv->bindings[statement.defineName] = value;

               if (value.isObject() || value.isBuiltin())
                  continue;

               Cell defineExpr(Cell::List);
               defineExpr.list.push_back(Cell(Cell::Symbol, "define"));
               defineExpr.list.push_back(Cell(Cell::Symbol, statement.defineName));
               defineExpr.list.push_back(requireExpr(value, "define"));
               normalized.list.push_back(defineExpr);

               globalEnv->bindings[statement.defineName] = Value::exprValue(Cell(Cell::Symbol, statement.defineName));
               continue;
            }

            normalized.list.push_back(requireExpr(eval(expr, globalEnv), "program result"));
         }

         return normalized;
      }

      Cell Evaluator::requireExpr(const Value &value, const std::string &context) const
      {
         if (value.isExpr())
            return value.expr;
         if (value.isNumber())
            return numberToCell(value.number);
         throw std::runtime_error(context + " cannot use a closure as a scalar/image expression");
      }

      Value Evaluator::imageValue(const gnine::Image &image)
      {
         return Value::objectValue(_heap.allocateImage(image));
      }

      Value Evaluator::imageValue(gnine::Image &&image)
      {
         return Value::objectValue(_heap.allocateImage(std::move(image)));
      }

      void Evaluator::clearExecutionTrace()
      {
         _executionTrace.clear();
         _reportedNonNumericPixel = false;
      }

      const std::vector<std::string> &Evaluator::executionTrace() const
      {
         return _executionTrace;
      }

      namespace
      {
         int requirePositiveCanvasInt(const Value &value, const std::string &context)
         {
            if (!value.isNumber())
               throw std::runtime_error(context + " expects numeric dimensions");

            double rounded = std::floor(value.number + 0.5);
            if (std::fabs(value.number - rounded) > 1e-9 || rounded <= 0.0)
               throw std::runtime_error(context + " expects positive integer dimensions");

            return static_cast<int>(rounded);
         }

         std::string describeValue(const Value &value)
         {
            if (value.isNumber())
            {
               std::ostringstream out;
               out << std::setprecision(17) << value.number;
               return out.str();
            }
            if (value.isExpr())
               return cellToString(value.expr);
            if (value.isBuiltin())
               return std::string("<builtin ") + value.builtinName + ">";
            if (value.isObject())
            {
               if (value.object->type == Object::Closure)
                  return "<closure>";
               if (value.object->type == Object::Image)
                  return "<image>";
               if (value.object->type == Object::Tuple)
                  return "<tuple>";
               if (value.object->type == Object::Environment)
                  return "<environment>";
            }
            return "<nil>";
         }

         std::string valueKindName(const Value &value)
         {
            if (value.isNumber())
               return "number";
            if (value.isExpr())
               return "expr";
            if (value.isBuiltin())
               return "builtin";
            if (value.isObject())
            {
               if (value.object->type == Object::Closure)
                  return "closure";
               if (value.object->type == Object::Image)
                  return "image";
               if (value.object->type == Object::Tuple)
                  return "tuple";
               if (value.object->type == Object::Environment)
                  return "environment";
            }
            return "nil";
         }
      }

      Heap &Evaluator::heap()
      {
         return _heap;
      }

      Value Evaluator::eval(const Cell &expr, EnvironmentObject *env)
      {
         if (expr.type == Cell::Number)
            return Value::numberValue(parseNumberLiteral(expr.val));

         if (expr.type == Cell::Symbol)
         {
            Value value;
            if (lookup(env, expr.val, &value))
               return value;
            if (_hasPixelContext)
            {
               if (expr.val == "i")
                  return Value::numberValue(_currentRow);
               if (expr.val == "j")
                  return Value::numberValue(_currentCol);
               if (expr.val == "c")
                  return Value::numberValue(_currentChannel);
            }
            if (isBuiltin(expr.val))
               return Value::builtinValue(expr.val);
            return Value::exprValue(expr);
         }

         if (expr.type != Cell::List || expr.list.empty())
            throw std::runtime_error("Invalid runtime expression");

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "lambda")
         {
            if (expr.list.size() != 3 || expr.list[1].type != Cell::List)
               throw std::runtime_error("lambda must be of form (lambda (x ...) body)");

            std::vector<Cell> paramPatterns;
            std::vector<std::string> params;
            for (size_t idx = 0; idx < expr.list[1].list.size(); ++idx)
            {
               const Cell &param = expr.list[1].list[idx];
               if (!isValidPattern(param))
                  throw std::runtime_error("lambda parameters must be symbols or tuple patterns");
               paramPatterns.push_back(param);
               if (param.type == Cell::Symbol)
                  params.push_back(param.val);
               else
                  params.push_back("");
            }
            return Value::objectValue(_heap.allocateClosure(params, paramPatterns, expr.list[2], env));
         }

         if (expr.list[0].type == Cell::Symbol &&
             (expr.list[0].val == "map-image" || expr.list[0].val == "zip-image"))
         {
            const std::string &name = expr.list[0].val;
            size_t imageArgCount = name == "map-image" ? 1 : 2;
            if (expr.list.size() != imageArgCount + 2)
               throw std::runtime_error(name + " expects " + std::to_string(imageArgCount + 1) + " arguments");

            std::vector<Value> args(imageArgCount, Value::nil());
            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.addRoot(&args[idx]);

            Value result = Value::nil();
            Heap::Root resultRoot(_heap, result);

            try
            {
               Value callee = eval(expr.list[1], env);
               Heap::Root calleeRoot(_heap, callee);
               for (size_t idx = 0; idx < args.size(); ++idx)
                  args[idx] = eval(expr.list[idx + 2], env);
               bool allImages = true;
               for (size_t idx = 0; idx < args.size(); ++idx)
               {
                  if (!args[idx].isObject() || args[idx].object->type != Object::Image)
                  {
                     allImages = false;
                     break;
                  }
               }

               if (allImages)
                  result = name == "map-image" ? applyMapImage(callee, args, name) : applyZipImage(callee, args, name);
               else
                  result = applyCallable(callee, args, name);
            }
            catch (...)
            {
               for (size_t idx = 0; idx < args.size(); ++idx)
                  _heap.removeRoot(&args[idx]);
               throw;
            }

            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.removeRoot(&args[idx]);
            return result;
         }

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "canvas")
         {
            if (expr.list.size() != 4 && expr.list.size() != 5)
               throw std::runtime_error("canvas expects (canvas width height body) or (canvas width height channels body)");

            Value widthValue = eval(expr.list[1], env);
            Heap::Root widthRoot(_heap, widthValue);
            Value heightValue = eval(expr.list[2], env);
            Heap::Root heightRoot(_heap, heightValue);

            int width = requirePositiveCanvasInt(widthValue, "canvas");
            int height = requirePositiveCanvasInt(heightValue, "canvas");
            int channels = 1;
            const Cell *body = &expr.list[3];

            if (expr.list.size() == 5)
            {
               Value channelValue = eval(expr.list[3], env);
               Heap::Root channelRoot(_heap, channelValue);
               channels = requirePositiveCanvasInt(channelValue, "canvas");
               body = &expr.list[4];
            }

            return applyCanvas(width, height, channels, *body, env, "canvas");
         }

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "if")
         {
            if (expr.list.size() != 4)
               throw std::runtime_error("if expects exactly three arguments");

            Value condition = eval(expr.list[1], env);
            Heap::Root conditionRoot(_heap, condition);
            if (condition.isNumber())
               return eval(condition.number != 0.0 ? expr.list[2] : expr.list[3], env);

            Value thenValue = eval(expr.list[2], env);
            Heap::Root thenRoot(_heap, thenValue);
            Value elseValue = eval(expr.list[3], env);
            Heap::Root elseRoot(_heap, elseValue);

            Cell residual(Cell::List);
            residual.list.push_back(Cell(Cell::Symbol, "if"));
            residual.list.push_back(requireExpr(condition, "if condition"));
            residual.list.push_back(requireExpr(thenValue, "if then-branch"));
            residual.list.push_back(requireExpr(elseValue, "if else-branch"));
            return Value::exprValue(residual);
         }

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "cond")
         {
            for (size_t clauseIdx = 1; clauseIdx < expr.list.size(); ++clauseIdx)
            {
               const Cell &clause = expr.list[clauseIdx];
               if (clause.type != Cell::List || clause.list.size() != 2)
                  throw std::runtime_error("cond clauses must be of form (test expr)");
               if (clause.list[0].type == Cell::Symbol && clause.list[0].val == "else")
                  return eval(clause.list[1], env);
               Value test = eval(clause.list[0], env);
               Heap::Root testRoot(_heap, test);
               if (test.isNumber())
               {
                  if (test.number != 0.0)
                     return eval(clause.list[1], env);
               }
               else
               {
                  Value thenVal = eval(clause.list[1], env);
                  Heap::Root thenRoot(_heap, thenVal);
                  Cell restCond(Cell::List);
                  restCond.list.push_back(Cell(Cell::Symbol, "cond"));
                  for (size_t restIdx = clauseIdx + 1; restIdx < expr.list.size(); ++restIdx)
                     restCond.list.push_back(expr.list[restIdx]);
                  Value elseVal = restCond.list.size() > 1 ? eval(restCond, env) : Value::nil();
                  Heap::Root elseRoot(_heap, elseVal);
                  Cell residual(Cell::List);
                  residual.list.push_back(Cell(Cell::Symbol, "if"));
                  residual.list.push_back(requireExpr(test, "cond condition"));
                  residual.list.push_back(requireExpr(thenVal, "cond then"));
                  residual.list.push_back(requireExpr(elseVal, "cond else"));
                  return Value::exprValue(residual);
               }
            }
            return Value::nil();
         }

         if (expr.list[0].type == Cell::Symbol &&
             (expr.list[0].val == "let" || expr.list[0].val == "let*"))
         {
            bool isStar = (expr.list[0].val == "let*");
            if (expr.list.size() != 3 || expr.list[1].type != Cell::List)
               throw std::runtime_error("let/let* must be of form (let ((x v) ...) body)");
            const Cell &bindingsList = expr.list[1];
            const Cell &letBody = expr.list[2];

            Value letEnvValue = Value::objectValue(_heap.allocateEnvironment(env));
            Heap::Root letEnvRoot(_heap, letEnvValue);
            EnvironmentObject *letEnv = static_cast<EnvironmentObject *>(letEnvValue.object);

            for (size_t bIdx = 0; bIdx < bindingsList.list.size(); ++bIdx)
            {
               const Cell &binding = bindingsList.list[bIdx];
               if (binding.type != Cell::List || binding.list.size() != 2 ||
                   binding.list[0].type != Cell::Symbol)
                  throw std::runtime_error("let binding must be of form (name expr)");
               Value bindVal = eval(binding.list[1], isStar ? letEnv : env);
               Heap::Root bindRoot(_heap, bindVal);
               letEnv->bindings[binding.list[0].val] = bindVal;
            }
            return eval(letBody, letEnv);
         }

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "begin")
         {
            if (expr.list.size() < 2)
               throw std::runtime_error("begin expects at least one expression");
            Value result = Value::nil();
            Heap::Root resultRoot(_heap, result);
            for (size_t bIdx = 1; bIdx < expr.list.size(); ++bIdx)
               result = eval(expr.list[bIdx], env);
            return result;
         }

         if (expr.list[0].type == Cell::Symbol &&
             (expr.list[0].val == "rgb" || expr.list[0].val == "vec"))
         {
            // In pixel context, select the channel-appropriate argument.
            // Outside pixel context (symbolic/normalize path), rebuild the
            // expression so VectorProgram lowering can see it.
            std::vector<Value> rgbArgs(expr.list.size() - 1, Value::nil());
            for (size_t idx = 0; idx < rgbArgs.size(); ++idx)
               _heap.addRoot(&rgbArgs[idx]);
            try
            {
               for (size_t idx = 1; idx < expr.list.size(); ++idx)
                  rgbArgs[idx - 1] = eval(expr.list[idx], env);
            }
            catch (...)
            {
               for (size_t idx = 0; idx < rgbArgs.size(); ++idx)
                  _heap.removeRoot(&rgbArgs[idx]);
               throw;
            }
            Value rgbResult = Value::nil();
            if (_hasPixelContext)
            {
               size_t ch = static_cast<size_t>(_currentChannel);
               if (ch < rgbArgs.size())
                  rgbResult = Value::numberValue(requireNumber(rgbArgs[ch], expr.list[0].val));
               else if (!rgbArgs.empty())
                  rgbResult = Value::numberValue(requireNumber(rgbArgs[0], expr.list[0].val));
               else
                  rgbResult = Value::numberValue(0.0);
            }
            else
            {
               // Rebuild symbolic expression so the VectorProgram / compiled
               // canvas channel-specialization path can process it.
               Cell rebuilt(Cell::List);
               rebuilt.list.push_back(expr.list[0]);
               bool anyNonNumber = false;
               for (size_t idx = 0; idx < rgbArgs.size(); ++idx)
                  if (!rgbArgs[idx].isNumber()) { anyNonNumber = true; break; }
               if (!anyNonNumber)
               {
                  for (size_t idx = 0; idx < rgbArgs.size(); ++idx)
                     rebuilt.list.push_back(doubleToCell(rgbArgs[idx].number));
               }
               else
               {
                  std::vector<Cell> exprArgs = requireExprArgs(rgbArgs, expr.list[0].val);
                  rebuilt.list.insert(rebuilt.list.end(), exprArgs.begin(), exprArgs.end());
               }
               rgbResult = Value::exprValue(rebuilt);
            }
            for (size_t idx = 0; idx < rgbArgs.size(); ++idx)
               _heap.removeRoot(&rgbArgs[idx]);
            return rgbResult;
         }

         std::vector<Value> args(expr.list.size() - 1, Value::nil());
         for (size_t idx = 0; idx < args.size(); ++idx)
            _heap.addRoot(&args[idx]);

         Value result = Value::nil();
         Heap::Root resultRoot(_heap, result);

         try
         {
            for (size_t idx = 1; idx < expr.list.size(); ++idx)
               args[idx - 1] = eval(expr.list[idx], env);
            Value callee = eval(expr.list[0], env);
            Heap::Root calleeRoot(_heap, callee);
            result = applyCallable(callee, args,
                                   expr.list[0].type == Cell::Symbol ? expr.list[0].val : std::string("<lambda>"));
         }
         catch (...)
         {
            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.removeRoot(&args[idx]);
            throw;
         }

         for (size_t idx = 0; idx < args.size(); ++idx)
            _heap.removeRoot(&args[idx]);
         return result;
      }

      Value Evaluator::applyBuiltin(const std::string &builtinName,
                                    const std::vector<Value> &args)
      {
         if (builtinName == "+")
         {
            double sum = 0.0;
            for (size_t idx = 0; idx < args.size(); ++idx)
               sum += requireNumber(args[idx], "+");
            return Value::numberValue(sum);
         }

         if (builtinName == "*")
         {
            double product = 1.0;
            for (size_t idx = 0; idx < args.size(); ++idx)
               product *= requireNumber(args[idx], "*");
            return Value::numberValue(product);
         }

         if (builtinName == "-")
         {
            if (args.empty())
               throw std::runtime_error("- expects at least one argument");
            double value = requireNumber(args[0], "-");
            if (args.size() == 1)
               return Value::numberValue(-value);
            for (size_t idx = 1; idx < args.size(); ++idx)
               value -= requireNumber(args[idx], "-");
            return Value::numberValue(value);
         }

         if (builtinName == "/")
         {
            if (args.empty())
               throw std::runtime_error("/ expects at least one argument");
            double value = requireNumber(args[0], "/");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value /= requireNumber(args[idx], "/");
            return Value::numberValue(value);
         }

         if (builtinName == "abs")
         {
            if (args.size() != 1)
               throw std::runtime_error("abs expects exactly one argument");
            return Value::numberValue(std::fabs(requireNumber(args[0], "abs")));
         }

         if (builtinName == "min")
         {
            if (args.empty())
               throw std::runtime_error("min expects at least one argument");
            double value = requireNumber(args[0], "min");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value = std::min(value, requireNumber(args[idx], "min"));
            return Value::numberValue(value);
         }

         if (builtinName == "max")
         {
            if (args.empty())
               throw std::runtime_error("max expects at least one argument");
            double value = requireNumber(args[0], "max");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value = std::max(value, requireNumber(args[idx], "max"));
            return Value::numberValue(value);
         }

         if (builtinName == "clamp")
         {
            if (args.size() != 3)
               throw std::runtime_error("clamp expects exactly three arguments");
            double value = requireNumber(args[0], "clamp");
            double low = requireNumber(args[1], "clamp");
            double high = requireNumber(args[2], "clamp");
            return Value::numberValue(std::min(std::max(value, low), high));
         }

         if (builtinName == "int")
         {
            if (args.size() != 1)
               throw std::runtime_error("int expects exactly one argument");
            return Value::numberValue(static_cast<long long>(requireNumber(args[0], "int")));
         }

         if (builtinName == "<")
         {
            if (args.size() != 2)
               throw std::runtime_error("< expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "<") < requireNumber(args[1], "<") ? 1.0 : 0.0);
         }

         if (builtinName == "<=")
         {
            if (args.size() != 2)
               throw std::runtime_error("<= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "<=") <= requireNumber(args[1], "<=") ? 1.0 : 0.0);
         }

         if (builtinName == ">")
         {
            if (args.size() != 2)
               throw std::runtime_error("> expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], ">") > requireNumber(args[1], ">") ? 1.0 : 0.0);
         }

         if (builtinName == ">=")
         {
            if (args.size() != 2)
               throw std::runtime_error(">= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], ">=") >= requireNumber(args[1], ">=") ? 1.0 : 0.0);
         }

         if (builtinName == "==")
         {
            if (args.size() != 2)
               throw std::runtime_error("== expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "==") == requireNumber(args[1], "==") ? 1.0 : 0.0);
         }

         if (builtinName == "!=")
         {
            if (args.size() != 2)
               throw std::runtime_error("!= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "!=") != requireNumber(args[1], "!=") ? 1.0 : 0.0);
         }

         if (builtinName == "and")
         {
            if (args.empty())
               throw std::runtime_error("and expects at least one argument");
            for (size_t idx = 0; idx < args.size(); ++idx)
            {
               if (requireNumber(args[idx], "and") == 0.0)
                  return Value::numberValue(0.0);
            }
            return Value::numberValue(1.0);
         }

         if (builtinName == "or")
         {
            if (args.empty())
               throw std::runtime_error("or expects at least one argument");
            for (size_t idx = 0; idx < args.size(); ++idx)
            {
               if (requireNumber(args[idx], "or") != 0.0)
                  return Value::numberValue(1.0);
            }
            return Value::numberValue(0.0);
         }

         if (builtinName == "not")
         {
            if (args.size() != 1)
               throw std::runtime_error("not expects exactly one argument");
            return Value::numberValue(requireNumber(args[0], "not") == 0.0 ? 1.0 : 0.0);
         }

         if (builtinName == "width")
         {
            if (args.size() != 1)
               throw std::runtime_error("width expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "width")->image->width());
         }

         if (builtinName == "height")
         {
            if (args.size() != 1)
               throw std::runtime_error("height expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "height")->image->height());
         }

         if (builtinName == "channels")
         {
            if (args.size() != 1)
               throw std::runtime_error("channels expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "channels")->image->channelCount());
         }

         if (builtinName == "sample-image")
            return applyAbsoluteImageSample(args, "sample-image");

         if (builtinName == "tuple")
            return Value::objectValue(_heap.allocateTuple(args));

         if (builtinName == "get")
         {
            if (args.size() != 2)
               throw std::runtime_error("get expects exactly two arguments");
            TupleObject *tuple = requireTupleObject(args[0], "get");
            int index = static_cast<int>(requireNumber(args[1], "get"));
            if (index < 0 || index >= static_cast<int>(tuple->values.size()))
               throw std::runtime_error("get index out of range");
            return tuple->values[index];
         }

         if (builtinName == "draw-rect")
         {
            if (args.size() != 5)
               throw std::runtime_error("draw-rect expects exactly five arguments (cx cy half_w half_h value)");
            if (!_hasPixelContext)
               throw std::runtime_error("draw-rect requires pixel context (canvas, map-image, or zip-image)");
            
            double cx = requireNumber(args[0], "draw-rect");
            double cy = requireNumber(args[1], "draw-rect");
            double half_w = requireNumber(args[2], "draw-rect");
            double half_h = requireNumber(args[3], "draw-rect");
            double value = requireNumber(args[4], "draw-rect");
            
            double i = static_cast<double>(_currentRow);
            double j = static_cast<double>(_currentCol);
            
            bool inside = (j >= cx - half_w) && (j <= cx + half_w) &&
                         (i >= cy - half_h) && (i <= cy + half_h);
            
            return Value::numberValue(inside ? value : 0.0);
         }

         if (builtinName == "draw-circle")
         {
            if (args.size() != 4)
               throw std::runtime_error("draw-circle expects exactly four arguments (cx cy radius value)");
            if (!_hasPixelContext)
               throw std::runtime_error("draw-circle requires pixel context (canvas, map-image, or zip-image)");
            
            double cx = requireNumber(args[0], "draw-circle");
            double cy = requireNumber(args[1], "draw-circle");
            double radius = requireNumber(args[2], "draw-circle");
            double value = requireNumber(args[3], "draw-circle");
            
            double i = static_cast<double>(_currentRow);
            double j = static_cast<double>(_currentCol);
            
            double dx = j - cx;
            double dy = i - cy;
            double dist_sq = dx * dx + dy * dy;
            double radius_sq = radius * radius;
            
            bool inside = dist_sq <= radius_sq;
            
            return Value::numberValue(inside ? value : 0.0);
         }

         if (builtinName == "mod")
         {
            if (args.size() != 2)
               throw std::runtime_error("mod expects exactly two arguments");
            double x = requireNumber(args[0], "mod");
            double n = requireNumber(args[1], "mod");
            if (n == 0.0)
               throw std::runtime_error("mod: division by zero");
            return Value::numberValue(x - n * std::floor(x / n));
         }

         if (builtinName == "floor")
         {
            if (args.size() != 1)
               throw std::runtime_error("floor expects exactly one argument");
            return Value::numberValue(std::floor(requireNumber(args[0], "floor")));
         }

         if (builtinName == "ceil")
         {
            if (args.size() != 1)
               throw std::runtime_error("ceil expects exactly one argument");
            return Value::numberValue(std::ceil(requireNumber(args[0], "ceil")));
         }

         if (builtinName == "sqrt")
         {
            if (args.size() != 1)
               throw std::runtime_error("sqrt expects exactly one argument");
            return Value::numberValue(std::sqrt(requireNumber(args[0], "sqrt")));
         }

         if (builtinName == "sin")
         {
            if (args.size() != 1)
               throw std::runtime_error("sin expects exactly one argument");
            return Value::numberValue(std::sin(requireNumber(args[0], "sin")));
         }

         if (builtinName == "cos")
         {
            if (args.size() != 1)
               throw std::runtime_error("cos expects exactly one argument");
            return Value::numberValue(std::cos(requireNumber(args[0], "cos")));
         }

         if (builtinName == "pow")
         {
            if (args.size() != 2)
               throw std::runtime_error("pow expects exactly two arguments");
            return Value::numberValue(std::pow(requireNumber(args[0], "pow"),
                                               requireNumber(args[1], "pow")));
         }

         if (builtinName == "atan2")
         {
            if (args.size() != 2)
               throw std::runtime_error("atan2 expects exactly two arguments");
            return Value::numberValue(std::atan2(requireNumber(args[0], "atan2"),
                                                 requireNumber(args[1], "atan2")));
         }

         if (builtinName == "rand-of")
         {
            if (args.size() != 1)
               throw std::runtime_error("rand-of expects exactly one argument");
            uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(requireNumber(args[0], "rand-of")));
            seed ^= seed >> 30;
            seed *= UINT64_C(0xbf58476d1ce4e5b9);
            seed ^= seed >> 27;
            seed *= UINT64_C(0x94d049bb133111eb);
            seed ^= seed >> 31;
            return Value::numberValue(static_cast<double>(seed >> 11) / static_cast<double>(UINT64_C(1) << 53));
         }

         if (builtinName == "draw-line")
         {
            if (args.size() != 6)
               throw std::runtime_error("draw-line expects exactly six arguments (x1 y1 x2 y2 thickness value)");
            if (!_hasPixelContext)
               throw std::runtime_error("draw-line requires pixel context (canvas, map-image, or zip-image)");
            double x1 = requireNumber(args[0], "draw-line");
            double y1 = requireNumber(args[1], "draw-line");
            double x2 = requireNumber(args[2], "draw-line");
            double y2 = requireNumber(args[3], "draw-line");
            double thickness = requireNumber(args[4], "draw-line");
            double value = requireNumber(args[5], "draw-line");
            double px = static_cast<double>(_currentCol);
            double py = static_cast<double>(_currentRow);
            double dx = x2 - x1;
            double dy = y2 - y1;
            double lenSq = dx * dx + dy * dy;
            double distSq;
            if (lenSq < 1e-12)
            {
               double ex = px - x1;
               double ey = py - y1;
               distSq = ex * ex + ey * ey;
            }
            else
            {
               double t = ((px - x1) * dx + (py - y1) * dy) / lenSq;
               if (t < 0.0) t = 0.0;
               if (t > 1.0) t = 1.0;
               double cx = x1 + t * dx;
               double cy = y1 + t * dy;
               double ex = px - cx;
               double ey = py - cy;
               distSq = ex * ex + ey * ey;
            }
            double halfThick = thickness * 0.5;
            return Value::numberValue(distSq <= halfThick * halfThick ? value : 0.0);
         }

         if (builtinName == "tuple-length")
         {
            if (args.size() != 1)
               throw std::runtime_error("tuple-length expects exactly one argument");
            return Value::numberValue(static_cast<double>(requireTupleObject(args[0], "tuple-length")->values.size()));
         }

         if (builtinName == "tuple-set")
         {
            if (args.size() != 3)
               throw std::runtime_error("tuple-set expects exactly three arguments (tuple idx val)");
            TupleObject *src = requireTupleObject(args[0], "tuple-set");
            int index = static_cast<int>(requireNumber(args[1], "tuple-set"));
            if (index < 0 || index >= static_cast<int>(src->values.size()))
               throw std::runtime_error("tuple-set index out of range");
            std::vector<Value> newValues(src->values);
            newValues[static_cast<size_t>(index)] = args[2];
            return Value::objectValue(_heap.allocateTuple(newValues));
         }

         if (builtinName == "tuple-slice")
         {
            if (args.size() != 3)
               throw std::runtime_error("tuple-slice expects exactly three arguments (tuple start end)");
            TupleObject *src = requireTupleObject(args[0], "tuple-slice");
            int start = static_cast<int>(requireNumber(args[1], "tuple-slice"));
            int end = static_cast<int>(requireNumber(args[2], "tuple-slice"));
            int sz = static_cast<int>(src->values.size());
            if (start < 0) start = 0;
            if (end > sz) end = sz;
            if (start > end) start = end;
            std::vector<Value> sliced(src->values.begin() + start, src->values.begin() + end);
            return Value::objectValue(_heap.allocateTuple(sliced));
         }

         throw std::runtime_error("Unsupported runtime builtin: " + builtinName);
      }

      Value Evaluator::applyCallable(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (callable.isObject())
         {
            if (callable.object->type == Object::Closure)
               return applyClosure(static_cast<ClosureObject *>(callable.object), args);
            if (callable.object->type == Object::Image)
               return applyImageSample(static_cast<ImageObject *>(callable.object), args, context);
            throw std::runtime_error(context + " cannot call this runtime object");
         }

         if (callable.isBuiltin())
        {
            if (allConcreteBuiltinArgs(args))
               return applyBuiltin(callable.builtinName, args);

            Value result = Value::nil();
            if (callable.builtinName == "if")
               throw std::runtime_error("if is a special form and cannot be passed as a function");

            if (!allNumbers(args))
            {
               const char *symbolicTraceEnv = std::getenv("GNINE_RUNTIME_TRACE_SYMBOLIC");
               if (!_reportedNonNumericPixel &&
                   symbolicTraceEnv != NULL &&
                   symbolicTraceEnv[0] != '\0' &&
                   std::string(symbolicTraceEnv) != "0")
               {
                  std::ostringstream out;
                  out << "runtime.builtin_symbolic builtin=" << callable.builtinName << " args=";
                  for (size_t idx = 0; idx < args.size(); ++idx)
                  {
                     if (idx > 0)
                        out << ",";
                     out << valueKindName(args[idx]) << ":" << describeValue(args[idx]);
                  }
                  traceExecution(out.str());
               }
               Cell expr(Cell::List);
               expr.list.push_back(Cell(Cell::Symbol, callable.builtinName));
               std::vector<Cell> exprArgs = requireExprArgs(args, context);
               expr.list.insert(expr.list.end(), exprArgs.begin(), exprArgs.end());
               result = Value::exprValue(expr);
            }

            if (!result.isNil())
               return result;
         }

         if (callable.isExpr())
         {
            if (callable.expr.type != Cell::Symbol)
               throw std::runtime_error(context + " requires a lambda or symbol in function position");

            Cell expr(Cell::List);
            expr.list.push_back(callable.expr);
            std::vector<Cell> exprArgs = requireExprArgs(args, context);
            expr.list.insert(expr.list.end(), exprArgs.begin(), exprArgs.end());
            return Value::exprValue(expr);
         }

         throw std::runtime_error(context + " call target is not callable");
      }

      void Evaluator::bindPattern(EnvironmentObject *env,
                                  const Cell &pattern,
                                  const Value &value)
      {
         if (pattern.type == Cell::Symbol)
         {
            env->bindings[pattern.val] = value;
            return;
         }

         if (pattern.type != Cell::List)
            throw std::runtime_error("Invalid runtime binding pattern");

         TupleObject *tuple = requireTupleObject(value, "tuple destructuring");
         if (tuple->values.size() != pattern.list.size())
            throw std::runtime_error("tuple destructuring arity mismatch");

         for (size_t idx = 0; idx < pattern.list.size(); ++idx)
            bindPattern(env, pattern.list[idx], tuple->values[idx]);
      }

      void Evaluator::bindPattern(EnvironmentObject *env,
                                  const Cell &pattern,
                                  const Value &value,
                                  const Value &symbolicValue,
                                  EnvironmentObject *symbolicEnv)
      {
         if (pattern.type == Cell::Symbol)
         {
            env->bindings[pattern.val] = value;
            symbolicEnv->bindings[pattern.val] = symbolicValue;
            return;
         }

         if (pattern.type != Cell::List)
            throw std::runtime_error("Invalid runtime binding pattern");

         TupleObject *tuple = requireTupleObject(value, "tuple destructuring");
         if (tuple->values.size() != pattern.list.size())
            throw std::runtime_error("tuple destructuring arity mismatch");
         Cell baseExpr = requireExpr(symbolicValue, "tuple destructuring");

         for (size_t idx = 0; idx < pattern.list.size(); ++idx)
         {
            Value elementSymbolic = Value::exprValue(tupleAccessExpr(baseExpr, idx));
            bindPattern(env, pattern.list[idx], tuple->values[idx], elementSymbolic, symbolicEnv);
         }
      }

      Value Evaluator::applyClosure(ClosureObject *closure,
                                    const std::vector<Value> &args)
      {
         if (closure->paramPatterns.size() != args.size())
            throw std::runtime_error("Closure expected " + std::to_string(closure->paramPatterns.size()) +
                                     " arguments but got " + std::to_string(args.size()));

         Value callEnvValue = Value::objectValue(_heap.allocateEnvironment(closure->env));
         Heap::Root callEnvRoot(_heap, callEnvValue);
         EnvironmentObject *callEnv = static_cast<EnvironmentObject *>(callEnvValue.object);

         for (size_t idx = 0; idx < args.size(); ++idx)
            bindPattern(callEnv, closure->paramPatterns[idx], args[idx]);
         if (_hasPixelContext)
         {
            callEnv->bindings["i"] = Value::numberValue(_currentRow);
            callEnv->bindings["j"] = Value::numberValue(_currentCol);
            callEnv->bindings["c"] = Value::numberValue(_currentChannel);
         }

         return eval(closure->body, callEnv);
      }

      Value Evaluator::applyImageSample(ImageObject *image,
                                        const std::vector<Value> &args,
                                        const std::string &context)
      {
         if (!_hasPixelContext)
            throw std::runtime_error(context + " image sampling requires map-image or zip-image pixel context");
         if (args.size() != 2)
            throw std::runtime_error(context + " image sampling expects exactly two offset arguments");

         int row = _currentRow + static_cast<int>(requireNumber(args[0], context));
         int col = _currentCol + static_cast<int>(requireNumber(args[1], context));
         row = reflectIndex(row, image->image->height());
         col = reflectIndex(col, image->image->width());
         int channel = image->image->channelCount() == 1 ? 0 : _currentChannel;
         return Value::numberValue(image->image->operator()(row, col, channel));
      }

      Value Evaluator::applyAbsoluteImageSample(const std::vector<Value> &args,
                                                const std::string &context)
      {
         if (args.size() != 3 && args.size() != 4)
            throw std::runtime_error(context + " expects (sample-image image x y) or (sample-image image x y c)");

         ImageObject *image = requireImageObject(args[0], context);
         int col = static_cast<int>(requireNumber(args[1], context));
         int row = static_cast<int>(requireNumber(args[2], context));
         int channel = 0;
         if (args.size() == 4)
            channel = static_cast<int>(requireNumber(args[3], context));

         row = reflectIndex(row, image->image->height());
         col = reflectIndex(col, image->image->width());
         if (image->image->channelCount() == 1)
            channel = 0;
         else
            channel = reflectIndex(channel, image->image->channelCount());

         return Value::numberValue(image->image->operator()(row, col, channel));
      }

      bool Evaluator::lookup(EnvironmentObject *env,
                             const std::string &name,
                             Value *outValue) const
      {
         for (EnvironmentObject *current = env; current != NULL; current = current->parent)
         {
            auto it = current->bindings.find(name);
            if (it != current->bindings.end())
            {
               *outValue = it->second;
               return true;
            }
         }
         return false;
      }

      bool Evaluator::isBuiltin(const std::string &name) const
      {
         return name == "+" || name == "-" || name == "*" || name == "/" ||
                name == "<" || name == "<=" || name == ">" || name == ">=" ||
                name == "==" || name == "!=" || name == "and" || name == "or" ||
                name == "not" || name == "min" || name == "max" || name == "abs" ||
                name == "clamp" || name == "int" || name == "tuple" || name == "get" ||
                name == "width" || name == "height" || name == "channels" || name == "sample-image" ||
                name == "draw-rect" || name == "draw-circle" ||
                name == "mod" || name == "floor" || name == "ceil" ||
                name == "sqrt" || name == "sin" || name == "cos" ||
                name == "pow" || name == "atan2" ||
                name == "rand-of" || name == "draw-line" || name == "begin" ||
                name == "tuple-length" || name == "tuple-set" || name == "tuple-slice";
      }

      bool Evaluator::allNumbers(const std::vector<Value> &args) const
      {
         for (size_t idx = 0; idx < args.size(); ++idx)
         {
            if (!args[idx].isNumber())
               return false;
         }
         return true;
      }

      bool Evaluator::allConcreteBuiltinArgs(const std::vector<Value> &args) const
      {
         for (size_t idx = 0; idx < args.size(); ++idx)
         {
            if (args[idx].isExpr())
               return false;
            if (args[idx].isObject() && args[idx].object->type == Object::Closure)
               return false;
         }
         return true;
      }

      double Evaluator::requireNumber(const Value &value,
                                      const std::string &context) const
      {
         if (!value.isNumber())
            throw std::runtime_error(context + " expects numeric arguments");
         return value.number;
      }

      ImageObject *Evaluator::requireImageObject(const Value &value,
                                                 const std::string &context) const
      {
         if (!value.isObject() || value.object->type != Object::Image)
            throw std::runtime_error(context + " expects image arguments");
         return static_cast<ImageObject *>(value.object);
      }

      TupleObject *Evaluator::requireTupleObject(const Value &value,
                                                 const std::string &context) const
      {
         if (!value.isObject() || value.object->type != Object::Tuple)
            throw std::runtime_error(context + " expects tuple arguments");
         return static_cast<TupleObject *>(value.object);
      }

      bool Evaluator::tryCompiledMapImage(ClosureObject *closure,
                                          const gnine::Image &source,
                                          Value *outResult,
                                          std::string *fallbackReason)
      {
         if (closure->paramPatterns.size() != 1 || closure->paramPatterns[0].type != Cell::Symbol)
         {
            if (fallbackReason != NULL)
               *fallbackReason = "closure_pattern";
            return false;
         }

         const std::string sourceArgName = "__runtime_source__";
         Cell argsCell(Cell::List);
         argsCell.list.push_back(makeSymbolCell(sourceArgName));

         Cell program(Cell::List);
         program.list.push_back(argsCell);
         if (closure->params[0] != sourceArgName)
            program.list.push_back(makeDefineCell(closure->params[0], makeSymbolCell(sourceArgName)));

         std::set<std::string> seenBindings;
         std::set<std::string> excludedNames;
         excludedNames.insert(closure->params[0]);
         excludedNames.insert("iter");
         std::vector<ScalarInputBinding> scalarInputs;
         std::vector<const gnine::Image *> inputImages;
         inputImages.push_back(&source);
         if (!appendCompiledReferencedBindings(closure->env, closure->body, excludedNames, &seenBindings,
                                               &scalarInputs, &inputImages, true, &argsCell, &program))
         {
            if (fallbackReason != NULL)
               *fallbackReason = "unsupported_capture";
            return false;
         }

         program.list[0] = argsCell;
         program.list.push_back(closure->body);

         if (!ensureRuntimeJitInitialized())
            throw std::runtime_error("Failed to initialize JIT for compiled runtime map-image");

         try
         {
            CompiledKernelLookup kernel;
            if (!lookupOrCompileRuntimeKernel(program, &kernel))
            {
               if (fallbackReason != NULL)
                  *fallbackReason = kernel.fallbackReason;
               return false;
            }

            ImageArrayFunctionType *fn = kernel.fn;
            int iterValue = runtimeIterValue(closure->env);
            gnine::Image resultImage(source.width(), source.height(), source.stride(), source.channelCount());
            while (_compiledScalarImages.size() < scalarInputs.size())
               _compiledScalarImages.push_back(gnine::Image(1, 1));
            for (size_t idx = 0; idx < scalarInputs.size(); ++idx)
            {
               gnine::Image &scalarImage = _compiledScalarImages[idx];
               *scalarImage.getChannelData(0) = scalarInputs[idx].value;
               inputImages.push_back(&scalarImage);
            }
            _scratchDataPtrs.resize(inputImages.size());
            _scratchInputWidths.resize(inputImages.size());
            _scratchInputHeights.resize(inputImages.size());
            _scratchInputStrides.resize(inputImages.size());
            for (size_t idx = 0; idx < inputImages.size(); ++idx)
            {
               const gnine::Image *image = inputImages[idx];
               _scratchInputWidths[idx] = image->width();
               _scratchInputHeights[idx] = image->height();
               _scratchInputStrides[idx] = image->stride();
            }
            for (int channel = 0; channel < source.channelCount(); ++channel)
            {
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  int sourceChannel = image->channelCount() == 1 ? 0 : channel;
                  _scratchDataPtrs[idx] = const_cast<double *>(image->getChannelData(sourceChannel));
               }
               fn(source.width(), source.height(), iterValue,
                  _scratchDataPtrs.data(),
                  _scratchInputWidths.data(),
                  _scratchInputHeights.data(),
                  _scratchInputStrides.data(),
                  resultImage.getChannelData(channel));
            }

            *outResult = imageValue(std::move(resultImage));
            if (fallbackReason != NULL)
            {
               *fallbackReason = std::string("cache=") + (kernel.cacheHit ? "hit" : "miss") +
                                 " compile_ms=" + formatMillis(kernel.compileMillis);
               const char *dumpEnv = std::getenv("GNINE_RUNTIME_TRACE_CACHE_KEY");
               if (!kernel.cacheHit &&
                   dumpEnv != NULL &&
                   dumpEnv[0] != '\0' &&
                   std::string(dumpEnv) != "0")
                  *fallbackReason += " dump=" + dumpRuntimeProgram(program);
            }
            return true;
         }
         catch (const std::exception &ex)
         {
            if (fallbackReason != NULL)
               *fallbackReason = std::string("exception what=") + ex.what();
            return false;
         }
      }

      bool Evaluator::tryCompiledZipImage(ClosureObject *closure,
                                          const gnine::Image &lhs,
                                          const gnine::Image &rhs,
                                          Value *outResult,
                                          std::string *fallbackReason)
      {
         if (closure->paramPatterns.size() != 2 ||
             closure->paramPatterns[0].type != Cell::Symbol ||
             closure->paramPatterns[1].type != Cell::Symbol)
         {
            if (fallbackReason != NULL)
               *fallbackReason = "closure_pattern";
            return false;
         }

         const std::string lhsArgName = "__runtime_lhs__";
         const std::string rhsArgName = "__runtime_rhs__";
         Cell argsCell(Cell::List);
         argsCell.list.push_back(makeSymbolCell(lhsArgName));
         argsCell.list.push_back(makeSymbolCell(rhsArgName));

         Cell program(Cell::List);
         program.list.push_back(argsCell);
         if (closure->params[0] != lhsArgName)
            program.list.push_back(makeDefineCell(closure->params[0], makeSymbolCell(lhsArgName)));
         if (closure->params[1] != rhsArgName)
            program.list.push_back(makeDefineCell(closure->params[1], makeSymbolCell(rhsArgName)));

         std::set<std::string> seenBindings;
         std::set<std::string> excludedNames;
         excludedNames.insert(closure->params[0]);
         excludedNames.insert(closure->params[1]);
         excludedNames.insert("iter");
         std::vector<ScalarInputBinding> scalarInputs;
         std::vector<const gnine::Image *> inputImages;
         inputImages.push_back(&lhs);
         inputImages.push_back(&rhs);
         if (!appendCompiledReferencedBindings(closure->env, closure->body, excludedNames, &seenBindings,
                                               &scalarInputs, &inputImages, true, &argsCell, &program))
         {
            if (fallbackReason != NULL)
               *fallbackReason = "unsupported_capture";
            return false;
         }

         program.list[0] = argsCell;
         program.list.push_back(closure->body);

         if (!ensureRuntimeJitInitialized())
            throw std::runtime_error("Failed to initialize JIT for compiled runtime zip-image");

         try
         {
            CompiledKernelLookup kernel;
            if (!lookupOrCompileRuntimeKernel(program, &kernel))
            {
               if (fallbackReason != NULL)
                  *fallbackReason = kernel.fallbackReason;
               return false;
            }

            ImageArrayFunctionType *fn = kernel.fn;
            int iterValue = runtimeIterValue(closure->env);
            const int resultChannels = std::max(lhs.channelCount(), rhs.channelCount());
            gnine::Image resultImage(lhs.width(), lhs.height(), lhs.stride(), resultChannels);
            while (_compiledScalarImages.size() < scalarInputs.size())
               _compiledScalarImages.push_back(gnine::Image(1, 1));
            for (size_t idx = 0; idx < scalarInputs.size(); ++idx)
            {
               gnine::Image &scalarImage = _compiledScalarImages[idx];
               *scalarImage.getChannelData(0) = scalarInputs[idx].value;
               inputImages.push_back(&scalarImage);
            }
            _scratchDataPtrs.resize(inputImages.size());
            _scratchInputWidths.resize(inputImages.size());
            _scratchInputHeights.resize(inputImages.size());
            _scratchInputStrides.resize(inputImages.size());
            for (size_t idx = 0; idx < inputImages.size(); ++idx)
            {
               const gnine::Image *image = inputImages[idx];
               _scratchInputWidths[idx] = image->width();
               _scratchInputHeights[idx] = image->height();
               _scratchInputStrides[idx] = image->stride();
            }
            for (int channel = 0; channel < resultChannels; ++channel)
            {
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  int sourceChannel = broadcastChannelIndex(*image, channel);
                  _scratchDataPtrs[idx] = const_cast<double *>(image->getChannelData(sourceChannel));
               }
               fn(lhs.width(), lhs.height(), iterValue,
                  _scratchDataPtrs.data(),
                  _scratchInputWidths.data(),
                  _scratchInputHeights.data(),
                  _scratchInputStrides.data(),
                  resultImage.getChannelData(channel));
            }

            *outResult = imageValue(std::move(resultImage));
            if (fallbackReason != NULL)
            {
               *fallbackReason = std::string("cache=") + (kernel.cacheHit ? "hit" : "miss") +
                                 " compile_ms=" + formatMillis(kernel.compileMillis);
               const char *dumpEnv = std::getenv("GNINE_RUNTIME_TRACE_CACHE_KEY");
               if (!kernel.cacheHit &&
                   dumpEnv != NULL &&
                   dumpEnv[0] != '\0' &&
                   std::string(dumpEnv) != "0")
                  *fallbackReason += " dump=" + dumpRuntimeProgram(program);
            }
            return true;
         }
         catch (const std::exception &ex)
         {
            if (fallbackReason != NULL)
               *fallbackReason = std::string("exception what=") + ex.what();
            return false;
         }
      }

      Value Evaluator::applyMapImage(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (args.size() != 1)
            throw std::runtime_error(context + " expects exactly one image");

         const gnine::Image &source = *requireImageObject(args[0], context)->image;
         if (callable.isObject() && callable.object->type == Object::Closure)
         {
            std::chrono::steady_clock::time_point compiledStart = std::chrono::steady_clock::now();
            Value compiledResult = Value::nil();
            std::string traceDetail;
            if (tryCompiledMapImage(static_cast<ClosureObject *>(callable.object), source, &compiledResult, &traceDetail))
            {
               std::chrono::steady_clock::time_point compiledEnd = std::chrono::steady_clock::now();
               traceExecution("runtime.map_image.mode=compiled " + traceDetail +
                              " execute_ms=" + formatMillis(elapsedMillis(compiledStart, compiledEnd)));
               return compiledResult;
            }
            std::chrono::steady_clock::time_point interpretedStart = std::chrono::steady_clock::now();
            traceExecution("runtime.map_image.compiled_fallback=" + traceDetail);
            gnine::Image resultImage(source.width(), source.height(), source.stride(), source.channelCount());
            for (int channel = 0; channel < source.channelCount(); ++channel)
            {
               for (int row = 0; row < source.height(); ++row)
               {
                  for (int col = 0; col < source.width(); ++col)
                  {
                     PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                                  row, col, channel);
                     std::vector<Value> pixelArgs(1, Value::numberValue(source(row, col, channel)));
                     Value pixelValue = applyCallable(callable, pixelArgs, context);
                     if (!pixelValue.isNumber() && !_reportedNonNumericPixel)
                     {
                        _reportedNonNumericPixel = true;
                        traceExecution("runtime.map_image.non_numeric row=" + std::to_string(row) +
                                       " col=" + std::to_string(col) +
                                       " channel=" + std::to_string(channel) +
                                       " value=" + describeValue(pixelValue));
                        if (callable.isObject() && callable.object->type == Object::Closure)
                           traceClosureBindings(static_cast<ClosureObject *>(callable.object),
                                               "runtime.map_image.",
                                               2);
                     }
                     resultImage(row, col, channel) = requireNumber(pixelValue, context);
                  }
               }
            }
            std::chrono::steady_clock::time_point interpretedEnd = std::chrono::steady_clock::now();
            traceExecution("runtime.map_image.mode=interpreted fallback=" + traceDetail +
                           " execute_ms=" + formatMillis(elapsedMillis(interpretedStart, interpretedEnd)));
            return imageValue(std::move(resultImage));
         }

         std::chrono::steady_clock::time_point interpretedStart = std::chrono::steady_clock::now();
         gnine::Image resultImage(source.width(), source.height(), source.stride(), source.channelCount());
         for (int channel = 0; channel < source.channelCount(); ++channel)
         {
            for (int row = 0; row < source.height(); ++row)
            {
               for (int col = 0; col < source.width(); ++col)
               {
                  PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                               row, col, channel);
                  std::vector<Value> pixelArgs(1, Value::numberValue(source(row, col, channel)));
                  Value pixelValue = applyCallable(callable, pixelArgs, context);
                  if (!pixelValue.isNumber() && !_reportedNonNumericPixel)
                  {
                     _reportedNonNumericPixel = true;
                     traceExecution("runtime.map_image.non_numeric row=" + std::to_string(row) +
                                    " col=" + std::to_string(col) +
                                    " channel=" + std::to_string(channel) +
                                    " value=" + describeValue(pixelValue));
                     if (callable.isObject() && callable.object->type == Object::Closure)
                        traceClosureBindings(static_cast<ClosureObject *>(callable.object),
                                            "runtime.map_image.",
                                            2);
                  }
                  resultImage(row, col, channel) = requireNumber(pixelValue, context);
               }
            }
         }
         std::chrono::steady_clock::time_point interpretedEnd = std::chrono::steady_clock::now();
         traceExecution("runtime.map_image.mode=interpreted fallback=non_closure execute_ms=" +
                        formatMillis(elapsedMillis(interpretedStart, interpretedEnd)));
         return imageValue(std::move(resultImage));
      }

      Value Evaluator::applyZipImage(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (args.size() != 2)
            throw std::runtime_error(context + " expects exactly two images");

         const gnine::Image &lhs = *requireImageObject(args[0], context)->image;
         const gnine::Image &rhs = *requireImageObject(args[1], context)->image;
         const int outputChannels = requireCompatibleZipImageChannels(lhs, rhs, context);

         if (callable.isObject() && callable.object->type == Object::Closure)
         {
            std::chrono::steady_clock::time_point compiledStart = std::chrono::steady_clock::now();
            Value compiledResult = Value::nil();
            std::string traceDetail;
            if (tryCompiledZipImage(static_cast<ClosureObject *>(callable.object), lhs, rhs, &compiledResult, &traceDetail))
            {
               std::chrono::steady_clock::time_point compiledEnd = std::chrono::steady_clock::now();
               traceExecution("runtime.zip_image.mode=compiled " + traceDetail +
                              " execute_ms=" + formatMillis(elapsedMillis(compiledStart, compiledEnd)));
               return compiledResult;
            }
            std::chrono::steady_clock::time_point interpretedStart = std::chrono::steady_clock::now();
            traceExecution("runtime.zip_image.compiled_fallback=" + traceDetail);
            gnine::Image resultImage(lhs.width(), lhs.height(), lhs.stride(), outputChannels);
            for (int channel = 0; channel < outputChannels; ++channel)
            {
               for (int row = 0; row < lhs.height(); ++row)
               {
                  for (int col = 0; col < lhs.width(); ++col)
                  {
                     PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                                  row, col, channel);
                     std::vector<Value> pixelArgs;
                     pixelArgs.push_back(Value::numberValue(lhs(row, col, broadcastChannelIndex(lhs, channel))));
                     pixelArgs.push_back(Value::numberValue(rhs(row, col, broadcastChannelIndex(rhs, channel))));
                     Value pixelValue = applyCallable(callable, pixelArgs, context);
                     if (!pixelValue.isNumber() && !_reportedNonNumericPixel)
                     {
                        _reportedNonNumericPixel = true;
                        traceExecution("runtime.zip_image.non_numeric row=" + std::to_string(row) +
                                       " col=" + std::to_string(col) +
                                       " channel=" + std::to_string(channel) +
                                       " value=" + describeValue(pixelValue));
                     }
                     resultImage(row, col, channel) = requireNumber(pixelValue, context);
                  }
               }
            }
            std::chrono::steady_clock::time_point interpretedEnd = std::chrono::steady_clock::now();
            traceExecution("runtime.zip_image.mode=interpreted fallback=" + traceDetail +
                           " execute_ms=" + formatMillis(elapsedMillis(interpretedStart, interpretedEnd)));
            return imageValue(std::move(resultImage));
         }

         std::chrono::steady_clock::time_point interpretedStart = std::chrono::steady_clock::now();
         gnine::Image resultImage(lhs.width(), lhs.height(), lhs.stride(), outputChannels);
         for (int channel = 0; channel < outputChannels; ++channel)
         {
            for (int row = 0; row < lhs.height(); ++row)
            {
               for (int col = 0; col < lhs.width(); ++col)
               {
                  PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                               row, col, channel);
                  std::vector<Value> pixelArgs;
                  pixelArgs.push_back(Value::numberValue(lhs(row, col, broadcastChannelIndex(lhs, channel))));
                  pixelArgs.push_back(Value::numberValue(rhs(row, col, broadcastChannelIndex(rhs, channel))));
                  Value pixelValue = applyCallable(callable, pixelArgs, context);
                  if (!pixelValue.isNumber() && !_reportedNonNumericPixel)
                  {
                     _reportedNonNumericPixel = true;
                     traceExecution("runtime.zip_image.non_numeric row=" + std::to_string(row) +
                                    " col=" + std::to_string(col) +
                                    " channel=" + std::to_string(channel) +
                                    " value=" + describeValue(pixelValue));
                  }
                  resultImage(row, col, channel) = requireNumber(pixelValue, context);
               }
            }
         }
         std::chrono::steady_clock::time_point interpretedEnd = std::chrono::steady_clock::now();
         traceExecution("runtime.zip_image.mode=interpreted fallback=non_closure execute_ms=" +
                        formatMillis(elapsedMillis(interpretedStart, interpretedEnd)));
         return imageValue(std::move(resultImage));
      }

      bool Evaluator::tryCompiledCanvas(int width,
                                        int height,
                                        int channels,
                                        const Cell &body,
                                        EnvironmentObject *env,
                                        Value *outResult,
                                        std::string *fallbackReason)
      {
         const char *disableCompiledEnv = std::getenv("GNINE_RUNTIME_DISABLE_COMPILED_CANVAS");
         if (disableCompiledEnv != NULL &&
             disableCompiledEnv[0] != '\0' &&
             disableCompiledEnv[0] != '0')
         {
            if (fallbackReason != NULL)
               *fallbackReason = "disabled_by_env";
            return false;
         }

         if (!ensureRuntimeJitInitialized())
            throw std::runtime_error("Failed to initialize JIT for compiled runtime canvas");

         try
         {
            int iterValue = runtimeIterValue(env);
            gnine::Image resultImage(width, height, width, channels);
            double totalCompileMillis = 0.0;
            bool allCacheHits = true;
            bool bodyHasMetadataCalls = containsMetadataBuiltinCall(body);
            const char *disableFusedEnv = std::getenv("GNINE_RUNTIME_DISABLE_FUSED_RGB_CANVAS");
            bool allowFusedRGB = disableFusedEnv == NULL || disableFusedEnv[0] == '\0' || disableFusedEnv[0] == '0';
            if (channels == 3 && allowFusedRGB)
            {
               std::set<std::string> excludedNames;
               excludedNames.insert("i");
               excludedNames.insert("j");
               excludedNames.insert("c");
               excludedNames.insert("iter");
               excludedNames.insert("width");
               excludedNames.insert("height");

               std::vector<Cell> specializedBodies;
               specializedBodies.reserve(3);
               Cell argsCell(Cell::List);
               Cell program(Cell::List);
               program.list.push_back(argsCell);
               std::set<std::string> seenBindings;
               std::vector<ScalarInputBinding> scalarInputs;
               std::vector<const gnine::Image *> inputImages;
               std::deque<gnine::Image> inputImageViews;

               for (int channel = 0; channel < 3; ++channel)
               {
                  Cell specializedBody;
                  std::set<std::string> symbols;
                  const std::vector<std::string> *cachedSymbols = NULL;
                  if (bodyHasMetadataCalls)
                  {
                     specializedBody = specializeSymbol(body, "c", doubleToCell(static_cast<double>(channel)));
                     specializedBody = specializeImageMetadataCalls(specializedBody, env);
                     collectReferencedSymbols(specializedBody, &symbols);
                  }
                  else
                  {
                     const CompiledCanvasChannelMetadata &metadata =
                         compiledCanvasChannelMetadata(body, channel);
                     specializedBody = metadata.specializedBody;
                     cachedSymbols = &metadata.referencedSymbols;
                  }
                  bool renamedImageBinding = false;
                  if (cachedSymbols != NULL)
                  {
                     for (size_t idx = 0; idx < cachedSymbols->size(); ++idx)
                     {
                        const std::string &symbol = (*cachedSymbols)[idx];
                        Value value;
                        const Cell *sourceExpr = NULL;
                        if (!findCompiledBinding(env, symbol, &value, &sourceExpr))
                           continue;
                        if (!value.isObject() || value.object->type != Object::Image)
                           continue;

                        const gnine::Image *image = static_cast<ImageObject *>(value.object)->image;
                        if (image->channelCount() <= 1)
                           continue;

                        std::string alias = symbol + "__c" + std::to_string(channel);
                        specializedBody = renameBindingRefs(specializedBody, symbol, alias);
                        renamedImageBinding = true;
                        if (seenBindings.insert(alias).second)
                        {
                           argsCell.list.push_back(makeSymbolCell(alias));
                           inputImageViews.push_back(gnine::Image(
                               const_cast<double *>(image->getChannelData(channel)),
                               image->width(),
                               image->height(),
                               image->stride(),
                               1));
                           inputImages.push_back(&inputImageViews.back());
                        }
                     }
                  }
                  else
                  {
                     for (std::set<std::string>::const_iterator it = symbols.begin(); it != symbols.end(); ++it)
                     {
                        Value value;
                        const Cell *sourceExpr = NULL;
                        if (!findCompiledBinding(env, *it, &value, &sourceExpr))
                           continue;
                        if (!value.isObject() || value.object->type != Object::Image)
                           continue;

                        const gnine::Image *image = static_cast<ImageObject *>(value.object)->image;
                        if (image->channelCount() <= 1)
                           continue;

                        std::string alias = *it + "__c" + std::to_string(channel);
                        specializedBody = renameBindingRefs(specializedBody, *it, alias);
                        renamedImageBinding = true;
                        if (seenBindings.insert(alias).second)
                        {
                           argsCell.list.push_back(makeSymbolCell(alias));
                           inputImageViews.push_back(gnine::Image(
                               const_cast<double *>(image->getChannelData(channel)),
                               image->width(),
                               image->height(),
                               image->stride(),
                               1));
                           inputImages.push_back(&inputImageViews.back());
                        }
                     }
                  }
                  if (renamedImageBinding)
                  {
                     symbols.clear();
                     collectReferencedSymbols(specializedBody, &symbols);
                     cachedSymbols = NULL;
                  }
                  bool appendedBindings =
                      cachedSymbols != NULL
                          ? appendCompiledReferencedBindings(env, specializedBody, *cachedSymbols,
                                                            excludedNames, &seenBindings,
                                                            &scalarInputs, &inputImages, true,
                                                            &argsCell, &program)
                          : appendCompiledReferencedBindings(env, specializedBody, symbols,
                                                            excludedNames, &seenBindings,
                                                            &scalarInputs, &inputImages, true,
                                                            &argsCell, &program);
                  if (!appendedBindings)
                  {
                     if (fallbackReason != NULL)
                        *fallbackReason = "unsupported_capture";
                     return false;
                  }
                  specializedBodies.push_back(specializedBody);
               }
               program.list[0] = argsCell;
               for (size_t idx = 0; idx < specializedBodies.size(); ++idx)
                  program.list.push_back(specializedBodies[idx]);

               CompiledRGBKernelLookup kernel;
               if (!lookupOrCompileRuntimeRGBKernel(program, &kernel))
               {
                  if (fallbackReason != NULL)
                     *fallbackReason = kernel.fallbackReason;
                  return false;
               }
               totalCompileMillis += kernel.compileMillis;
               allCacheHits = allCacheHits && kernel.cacheHit;

               while (_compiledScalarImages.size() < scalarInputs.size())
                  _compiledScalarImages.push_back(gnine::Image(1, 1));

               for (size_t idx = 0; idx < scalarInputs.size(); ++idx)
               {
                  gnine::Image &scalarImage = _compiledScalarImages[idx];
                  *scalarImage.getChannelData(0) = scalarInputs[idx].value;
                  inputImages.push_back(&scalarImage);
               }

               _scratchDataPtrs.resize(inputImages.size());
               _scratchInputWidths.resize(inputImages.size());
               _scratchInputHeights.resize(inputImages.size());
               _scratchInputStrides.resize(inputImages.size());
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  _scratchDataPtrs[idx] = const_cast<double *>(image->getChannelData(0));
                  _scratchInputWidths[idx] = image->width();
                  _scratchInputHeights[idx] = image->height();
                  _scratchInputStrides[idx] = image->stride();
               }

               kernel.fn(width, height, iterValue,
                         _scratchDataPtrs.data(),
                         _scratchInputWidths.data(),
                         _scratchInputHeights.data(),
                         _scratchInputStrides.data(),
                         resultImage.getChannelData(0),
                         resultImage.getChannelData(1),
                         resultImage.getChannelData(2));

               *outResult = imageValue(std::move(resultImage));
               if (fallbackReason != NULL)
                  *fallbackReason = std::string("cache=") + (allCacheHits ? "hit" : "miss") +
                                    " compile_ms=" + formatMillis(totalCompileMillis) +
                                    " fused=rgb";
               return true;
            }

            for (int channel = 0; channel < channels; ++channel)
            {
               Cell specializedBody;
               std::set<std::string> symbols;
               const std::vector<std::string> *cachedSymbols = NULL;
               if (bodyHasMetadataCalls)
               {
                  specializedBody = specializeSymbol(body, "c", doubleToCell(static_cast<double>(channel)));
                  specializedBody = specializeImageMetadataCalls(specializedBody, env);
                  collectReferencedSymbols(specializedBody, &symbols);
               }
               else
               {
                  const CompiledCanvasChannelMetadata &metadata =
                      compiledCanvasChannelMetadata(body, channel);
                  specializedBody = metadata.specializedBody;
                  cachedSymbols = &metadata.referencedSymbols;
               }
               Cell argsCell(Cell::List);
               Cell program(Cell::List);
               program.list.push_back(argsCell);

               std::set<std::string> seenBindings;
               std::set<std::string> excludedNames;
               excludedNames.insert("i");
               excludedNames.insert("j");
               excludedNames.insert("c");
               excludedNames.insert("iter");
               excludedNames.insert("width");
               excludedNames.insert("height");
               std::vector<ScalarInputBinding> scalarInputs;
               std::vector<const gnine::Image *> inputImages;

               bool appendedBindings =
                   cachedSymbols != NULL
                       ? appendCompiledReferencedBindings(env, specializedBody, *cachedSymbols,
                                                         excludedNames, &seenBindings,
                                                         &scalarInputs, &inputImages, true,
                                                         &argsCell, &program)
                       : appendCompiledReferencedBindings(env, specializedBody, symbols,
                                                         excludedNames, &seenBindings,
                                                         &scalarInputs, &inputImages, true,
                                                         &argsCell, &program);
               if (!appendedBindings)
               {
                  if (fallbackReason != NULL)
                     *fallbackReason = "unsupported_capture";
                  return false;
               }

               program.list[0] = argsCell;
               program.list.push_back(specializedBody);

               CompiledKernelLookup kernel;
               if (!lookupOrCompileRuntimeKernel(program, &kernel))
               {
                  if (fallbackReason != NULL)
                     *fallbackReason = kernel.fallbackReason;
                  return false;
               }
               totalCompileMillis += kernel.compileMillis;
               allCacheHits = allCacheHits && kernel.cacheHit;

               while (_compiledScalarImages.size() < scalarInputs.size())
                  _compiledScalarImages.push_back(gnine::Image(1, 1));

               for (size_t idx = 0; idx < scalarInputs.size(); ++idx)
               {
                  gnine::Image &scalarImage = _compiledScalarImages[idx];
                  *scalarImage.getChannelData(0) = scalarInputs[idx].value;
                  inputImages.push_back(&scalarImage);
               }

               _scratchDataPtrs.resize(inputImages.size());
               _scratchInputWidths.resize(inputImages.size());
               _scratchInputHeights.resize(inputImages.size());
               _scratchInputStrides.resize(inputImages.size());
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  int sourceChannel = image->channelCount() == 1 ? 0 : channel;
                  _scratchDataPtrs[idx] = const_cast<double *>(image->getChannelData(sourceChannel));
                  _scratchInputWidths[idx] = image->width();
                  _scratchInputHeights[idx] = image->height();
                  _scratchInputStrides[idx] = image->stride();
               }
               kernel.fn(width, height, iterValue,
                         _scratchDataPtrs.data(),
                         _scratchInputWidths.data(),
                         _scratchInputHeights.data(),
                         _scratchInputStrides.data(),
                         resultImage.getChannelData(channel));
            }

            *outResult = imageValue(std::move(resultImage));
            if (fallbackReason != NULL)
               *fallbackReason = std::string("cache=") + (allCacheHits ? "hit" : "miss") +
                                 " compile_ms=" + formatMillis(totalCompileMillis);
            return true;
         }
         catch (const std::exception &ex)
         {
            if (fallbackReason != NULL)
               *fallbackReason = std::string("exception what=") + ex.what();
            return false;
         }
      }

      Value Evaluator::applyCanvas(int width,
                                   int height,
                                   int channels,
                                   const Cell &body,
                                   EnvironmentObject *env,
                                   const std::string &context)
      {
         // Try JIT-compiled version first
         std::chrono::steady_clock::time_point compiledStart = std::chrono::steady_clock::now();
         Value compiledResult = Value::nil();
         std::string traceDetail;
         if (tryCompiledCanvas(width, height, channels, body, env, &compiledResult, &traceDetail))
         {
            std::chrono::steady_clock::time_point compiledEnd = std::chrono::steady_clock::now();
            traceExecution("runtime.canvas.mode=compiled width=" + std::to_string(width) +
                           " height=" + std::to_string(height) +
                           " channels=" + std::to_string(channels) +
                           " " + traceDetail +
                           " execute_ms=" + formatMillis(elapsedMillis(compiledStart, compiledEnd)));
            return compiledResult;
         }

         std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
         gnine::Image resultImage(width, height, width, channels);
         for (int channel = 0; channel < channels; ++channel)
         {
            for (int row = 0; row < height; ++row)
            {
               for (int col = 0; col < width; ++col)
               {
                  PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                               row, col, channel);
                  Value pixelValue = eval(body, env);
                  if (!pixelValue.isNumber() && !_reportedNonNumericPixel)
                  {
                     _reportedNonNumericPixel = true;
                     traceExecution("runtime.canvas.non_numeric row=" + std::to_string(row) +
                                    " col=" + std::to_string(col) +
                                    " channel=" + std::to_string(channel) +
                                    " value=" + describeValue(pixelValue));
                  }
                  resultImage(row, col, channel) = requireNumber(pixelValue, context);
               }
            }
         }

         std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
         traceExecution("runtime.canvas.mode=interpreted fallback=" + traceDetail +
                        " width=" + std::to_string(width) +
                        " height=" + std::to_string(height) +
                        " channels=" + std::to_string(channels) +
                        " execute_ms=" + formatMillis(elapsedMillis(start, end)));
         return imageValue(std::move(resultImage));
      }

      void Evaluator::traceExecution(const std::string &entry)
      {
         _executionTrace.push_back(entry);
         const char *traceEnv = std::getenv("GNINE_RUNTIME_TRACE");
         if (traceEnv != NULL && traceEnv[0] != '\0' && std::string(traceEnv) != "0")
            std::cerr << entry << std::endl;
      }

      void Evaluator::traceEnvironmentBindings(EnvironmentObject *env,
                                               const std::string &tracePrefix,
                                               int maxClosureDepth)
      {
         std::set<const EnvironmentObject *> seenEnvs;
         std::deque<std::pair<EnvironmentObject *, std::string> > pending;
         pending.push_back(std::make_pair(env, tracePrefix));
         while (!pending.empty())
         {
            EnvironmentObject *currentEnv = pending.front().first;
            std::string currentPrefix = pending.front().second;
            pending.pop_front();
            if (currentEnv == NULL || !seenEnvs.insert(currentEnv).second)
               continue;

            for (auto it = currentEnv->bindings.begin();
                 it != currentEnv->bindings.end();
                 ++it)
            {
               traceExecution(currentPrefix + it->first +
                              "=" + valueKindName(it->second) +
                              " value=" + describeValue(it->second));
               if (maxClosureDepth > 0 &&
                   it->second.isObject() &&
                   it->second.object->type == Object::Closure)
               {
                  traceClosureBindings(static_cast<ClosureObject *>(it->second.object),
                                      currentPrefix + it->first + ".",
                                      maxClosureDepth - 1);
               }
            }

            if (currentEnv->parent != NULL)
               pending.push_back(std::make_pair(currentEnv->parent, currentPrefix + "parent."));
         }
      }

      void Evaluator::traceClosureBindings(ClosureObject *closure,
                                           const std::string &tracePrefix,
                                           int maxClosureDepth)
      {
         if (closure == NULL)
            return;

         std::ostringstream out;
         out << tracePrefix << "closure.params=";
         for (size_t idx = 0; idx < closure->params.size(); ++idx)
         {
            if (idx != 0)
               out << ",";
            out << (closure->params[idx].empty() ? "<pattern>" : closure->params[idx]);
         }
         traceExecution(out.str());
         traceExecution(tracePrefix + "closure.body=" + cellToString(closure->body));
         traceEnvironmentBindings(closure->env, tracePrefix + "env.", maxClosureDepth);
      }

      Cell Evaluator::numberToCell(double value) const
      {
         std::ostringstream out;
         out << std::setprecision(17) << value;
         return Cell(Cell::Number, out.str());
      }

      std::vector<Cell> Evaluator::requireExprArgs(const std::vector<Value> &args,
                                                   const std::string &context) const
      {
         std::vector<Cell> exprArgs;
         exprArgs.reserve(args.size());
         for (size_t idx = 0; idx < args.size(); ++idx)
            exprArgs.push_back(requireExpr(args[idx], context));
         return exprArgs;
      }
   }
}
